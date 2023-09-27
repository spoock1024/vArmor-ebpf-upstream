// SPDX-License-Identifier: GPL-2.0
// Copyright 2023 vArmor-ebpf Authors

#ifndef __ENFORCER_H
#define __ENFORCER_H

#include "vmlinux.h"
#include "bpf_helpers.h"
#include "bpf_tracing.h"
#include "bpf_core_read.h"

// #define DEBUG 1
#ifdef DEBUG
#define DEBUG_PRINT(fmt, args...) \
  bpf_printk(fmt, ##args)
#else
#define DEBUG_PRINT(fmt, args...) \
  do { } while (0)
#endif

#undef container_of
#define container_of(ptr, type, member)                                        \
  ({                                                                           \
    const typeof(((type *)0)->member) *__mptr = (ptr);                         \
    (type *)((char *)__mptr - offsetof(type, member));                         \
  })

#define	EPERM 1
#define OUTER_MAP_ENTRIES_MAX 100
#define FILE_PATH_PATTERN_SIZE_MAX 64
#define BUFFER_MAX 4096*3
#define NAME_MAX 256
#define PATH_MAX 4096
#define PATH_DEPTH_MAX 30
#define PRECISE_MATCH 0x00000001
#define GREEDY_MATCH  0x00000002
#define PREFIX_MATCH  0x00000004
#define SUFFIX_MATCH  0x00000008
#define CIDR_MATCH    0x00000020
#define IPV4_MATCH    0x00000040
#define IPV6_MATCH    0x00000080
#define PORT_MATCH    0x00000100

/*
  We use the buffer to cache file path and file name etc.
  |---------------------------------------|---------------------------------------|---------------------------------------|
  |                                       |                                       |                                       |
  |                              file path|                                       |file name                              | file_open()
  |                                 path-1|                                 path-2|name-1   |name-2                       | path_symlink(), path_link(), path_rename()
  |exec path                              |                                       |exec name|                             | bprm_check_security()
  |dev path                               |                                       |dev name |                    |fstype  | mount()
  |                              from path|                                       |from name|                             | move_mount()
  |                                       |                                       |                                       |
  |---------------------------------------|---------------------------------------|---------------------------------------|

  |------------------4096-----------------|------------------4096-----------------|---256---|---256---| |---16---|---16---|
*/
struct buffer {
  unsigned char value[BUFFER_MAX];
};

struct {
  __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
  __type(key, u32);
  __type(value, struct buffer);
  __uint(max_entries, 1);
} v_buffer SEC(".maps");

struct buffer_offset {
  u32 first_path;
  u32 first_name;
  u32 second_path;
  u32 second_name;
};

struct path_pattern {
  u32 flags;
  unsigned char prefix[FILE_PATH_PATTERN_SIZE_MAX];
  unsigned char suffix[FILE_PATH_PATTERN_SIZE_MAX];
};

static struct buffer *get_buffer() {
    int index = 0;
    return bpf_map_lookup_elem(&v_buffer, &index);
}

static u32 get_task_mnt_ns_id(struct task_struct *task) {
  return BPF_CORE_READ(task, nsproxy, mnt_ns, ns).inum;
}

static struct user_namespace *get_task_user_ns(struct task_struct *task) {
  return BPF_CORE_READ(task, cred, user_ns);
}

static kernel_cap_t get_task_cap_effective(struct task_struct *task) {
  return BPF_CORE_READ(task, cred, cap_effective);
}

// static __noinline u32 get_task_uts_ns_id(struct task_struct *task) {
//   return BPF_CORE_READ(task, nsproxy, uts_ns, ns).inum;
// }

// static struct file *get_task_exe_file(struct task_struct *task) {
//   return BPF_CORE_READ(task, mm, exe_file);
// }

static int task_in_execve(struct task_struct *task) {
  unsigned long long val = 0;
  unsigned int offset = __builtin_preserve_field_info(task->in_execve, BPF_FIELD_BYTE_OFFSET);
  unsigned int size = __builtin_preserve_field_info(task->in_execve, BPF_FIELD_BYTE_SIZE);
  bpf_probe_read(&val, size, (void *)task + offset);
  val <<= __builtin_preserve_field_info(task->in_execve, BPF_FIELD_LSHIFT_U64);
  val >>= __builtin_preserve_field_info(task->in_execve, BPF_FIELD_RSHIFT_U64);
  return (int)val;
}

static inline struct mount *real_mount(struct vfsmount *mnt) {
  return container_of(mnt, struct mount, mnt);
}

// prepend_path_to_first_block - parse the file path to the first block but ignores chroot'ed root.
static __noinline int prepend_path_to_first_block(struct dentry *dentry, struct vfsmount *vfsmnt, struct buffer *buf, struct buffer_offset *buf_offset) {
  struct mount *mnt = real_mount(vfsmnt);
  struct dentry *parent;
  struct dentry *mnt_root;
  struct mount *m;
  struct qstr d_name;

  char slash = '/';
  char null = '\0';
  int offset = PATH_MAX;

  #pragma unroll
  for (int i = 0; i < PATH_DEPTH_MAX; i++) {
    parent = BPF_CORE_READ(dentry, d_parent);
    mnt_root = BPF_CORE_READ(vfsmnt, mnt_root);

    if (dentry == mnt_root) {
      m = BPF_CORE_READ(mnt, mnt_parent);
      if (mnt != m) {
        dentry = BPF_CORE_READ(mnt, mnt_mountpoint);
        mnt = m;
        continue;
      }
      break;
    }

    if (dentry == parent) {
      break;
    }

    d_name = BPF_CORE_READ(dentry, d_name);

    offset -= (d_name.len + 1);
    if (offset < 0)
      break;

    int ret = bpf_probe_read(
                  &(buf->value[offset & (PATH_MAX - 1)]),
                  d_name.len & (NAME_MAX - 1), 
                  d_name.name);
    if (ret == 0) {
      bpf_probe_read(
          &(buf->value[(offset + d_name.len) & (PATH_MAX - 1)]),
          1,
          &slash);

      // cache the file name to the 3nd block of buffer
      if (buf_offset->first_name == 0) {
        bpf_probe_read(
                  &(buf->value[PATH_MAX*2]),
                  d_name.len & (NAME_MAX - 1),
                  d_name.name);
        bpf_probe_read(&(buf->value[(PATH_MAX*2 + d_name.len) & (PATH_MAX*3 - 1)]), 1, &null);
        buf_offset->first_name = d_name.len;

      }
    } else {
      offset += (d_name.len + 1);
    }

    dentry = parent;
  }

  // the path must end with '\0'
  if (offset == PATH_MAX) {
    offset--;
  }
  bpf_probe_read(&(buf->value[PATH_MAX - 1]), 1, &null);

  // the path must start with '/'
  offset--;
  bpf_probe_read(&(buf->value[offset & (PATH_MAX - 1)]), 1, &slash);

  // struct buffer *buf_test = get_file_buffer_test();
  // if (buf_test == 0)
  //   return PATH_MAX;
  // bpf_probe_read_str(buf_test->value, PATH_MAX, &(buf->value[offset & (PATH_MAX - 1)]));
  // DEBUG_PRINT("%s", buf_test->value);

  buf_offset->first_path = offset;
  return 0;
}

// prepend_path_to_second_block - parse the file path to the second block but ignores chroot'ed root.
static __noinline int prepend_path_to_second_block(struct dentry *dentry, struct vfsmount *vfsmnt, struct buffer *buf, struct buffer_offset *buf_offset) {
  struct mount *mnt = real_mount(vfsmnt);
  struct dentry *parent;
  struct dentry *mnt_root;
  struct mount *m;
  struct qstr d_name;

  char slash = '/';
  char null = '\0';
  int offset = PATH_MAX*2;

  #pragma unroll
  for (int i = 0; i < PATH_DEPTH_MAX; i++) {
    parent = BPF_CORE_READ(dentry, d_parent);
    mnt_root = BPF_CORE_READ(vfsmnt, mnt_root);

    if (dentry == mnt_root) {
      m = BPF_CORE_READ(mnt, mnt_parent);
      if (mnt != m) {
        dentry = BPF_CORE_READ(mnt, mnt_mountpoint);
        mnt = m;
        continue;
      }
      break;
    }

    if (dentry == parent) {
      break;
    }

    d_name = BPF_CORE_READ(dentry, d_name);

    offset -= (d_name.len + 1);
    if (offset < 0)
      break;

    int ret = bpf_probe_read(
                  &(buf->value[offset & (PATH_MAX*2 - 1)]),
                  d_name.len & (NAME_MAX - 1), 
                  d_name.name);
    if (ret == 0) {
      bpf_probe_read(
          &(buf->value[(offset + d_name.len) & (PATH_MAX*2 - 1)]),
          1,
          &slash);

      // cache the file name to the 3nd block of buffer
      if (buf_offset->second_name == 0) {
        bpf_probe_read(
                  &(buf->value[(PATH_MAX*2 + NAME_MAX) & (PATH_MAX*3 - 1)]),
                  d_name.len & (NAME_MAX - 1),
                  d_name.name);

        bpf_probe_read(&(buf->value[(PATH_MAX*2 + NAME_MAX + d_name.len) & (PATH_MAX*3 - 1)]), 1, &null);
        buf_offset->second_name = d_name.len;
      }
    } else {
      offset += (d_name.len + 1);
    }

    dentry = parent;
  }

  // the path must end with '\0'
  if (offset == PATH_MAX*2) {
    offset--;
  }
  bpf_probe_read(&(buf->value[PATH_MAX*2 - 1]), 1, &null);

  // the path must start with '/'
  offset--;
  bpf_probe_read(&(buf->value[offset & (PATH_MAX*2 - 1)]), 1, &slash);

  buf_offset->second_path = offset;
  return 0;
}

// prepend_string_to_first_block - copy the string to the first block
static __noinline int prepend_string_to_first_block(const char *string, struct buffer *buf, struct buffer_offset *buf_offset) {
  int ret = bpf_probe_read_kernel_str(buf->value, PATH_MAX, string);
  if (ret >= 0) {
    buf_offset->first_path = ret;
  } else {
    return -1;
  }

  int index = 0;
  for (; index < NAME_MAX; index++) {
    if (buf->value[(buf_offset->first_path - 1 - index) & (PATH_MAX - 1)] == '/')
      break;
  }

  if (index != 0 && index != NAME_MAX) {
    ret = bpf_probe_read_kernel_str(&(buf->value[PATH_MAX*2]), NAME_MAX, &(buf->value[(buf_offset->first_path - 1 - index + 1) & (PATH_MAX - 1)]));
    if (ret > 0)
      buf_offset->first_name = ret - 1;
  }

  return 0;
}


static __noinline bool is_prefix_match(unsigned char *prefix, unsigned char *path) {
  for (int i = 0; i < FILE_PATH_PATTERN_SIZE_MAX; i++) {
    if (prefix[i] == '\0')
      break;

    if (prefix[i] != path[i])
      return false;
  }

  return true;
}

static __noinline bool is_suffix_match(unsigned char *suffix, unsigned char *path, int offset) {
  for (int i = 0; i < FILE_PATH_PATTERN_SIZE_MAX; i++) {
    if (suffix[i] == '\0')
      break;

    if (suffix[i] != path[(offset - i) & (PATH_MAX-1)])
      return false;
  }

  return true;
}

static __always_inline bool old_path_check(struct path_pattern *pattern, struct buffer *buf, struct buffer_offset *offset) {

  DEBUG_PRINT("old_path_check() - pattern flags: 0x%x", pattern->flags);

  bool match = true;
  if (pattern->flags & GREEDY_MATCH || pattern->flags & PRECISE_MATCH) {
    // precise match or greedy match for the globbing "**" with file path
    DEBUG_PRINT("old_path_check() - matching path");

    if (pattern->flags & PREFIX_MATCH) {
      DEBUG_PRINT("old_path_check() - pattern prefix: %s", pattern->prefix);
      if (is_prefix_match(pattern->prefix, &(buf->value[offset->first_path & (PATH_MAX - 1)]))) {
        match = true;
      } else {
        match = false;
      }
    }

    if ((pattern->flags & SUFFIX_MATCH) && match) {
      DEBUG_PRINT("old_path_check() - pattern suffix: %s", pattern->suffix);
      if (is_suffix_match(pattern->suffix, buf->value, PATH_MAX - 2)) {
        match = true;
      } else {
        match = false;
      }
    }
  } else {
    // non-greedy match for the globbing "*" with file name
    DEBUG_PRINT("old_path_check() - matching name");

    if (pattern->flags & PREFIX_MATCH) {
      DEBUG_PRINT("old_path_check() - pattern prefix: %s", pattern->prefix);
      if (is_prefix_match(pattern->prefix, &(buf->value[PATH_MAX * 2]))) {
        match = true;
      } else {
        match = false;
      }
    }

    if ((pattern->flags & SUFFIX_MATCH) && match) {
      DEBUG_PRINT("old_path_check() - pattern suffix: %s", pattern->suffix);
      if (is_suffix_match(pattern->suffix, buf->value + PATH_MAX*2, offset->first_name - 1)) {
        match = true;
      } else {
        match = false;
      }
    }
  }

  return match;
}

static __noinline bool new_path_check(struct path_pattern *pattern, struct buffer *buf, struct buffer_offset *offset) {

  DEBUG_PRINT("new_path_check() - pattern flags: 0x%x", pattern->flags);

  bool match = true;
  if (pattern->flags & GREEDY_MATCH || pattern->flags & PRECISE_MATCH) {
    // precise match or greedy match for the globbing "**" with file path
    DEBUG_PRINT("new_path_check() - matching path");

    if (pattern->flags & PREFIX_MATCH) {
      DEBUG_PRINT("new_path_check() - pattern prefix: %s", pattern->prefix);
      if (is_prefix_match(pattern->prefix, &(buf->value[offset->second_path & (PATH_MAX*2 - 1)]))) {
        match = true;
      } else {
        match = false;
      }
    }

    if ((pattern->flags & SUFFIX_MATCH) && match) {
      DEBUG_PRINT("new_path_check() - pattern suffix: %s", pattern->suffix);
      if (is_suffix_match(pattern->suffix, buf->value + PATH_MAX, PATH_MAX*2 - 2)) {
        match = true;
      } else {
        match = false;
      }
    }
  } else {
    // non-greedy match for the globbing "*" with file name
    DEBUG_PRINT("new_path_check() - matching name");

    if (pattern->flags & PREFIX_MATCH) {
      DEBUG_PRINT("new_path_check() - pattern prefix: %s", pattern->prefix);
      if (is_prefix_match(pattern->prefix, &(buf->value[PATH_MAX*2 + NAME_MAX]))) {
        match = true;
      } else {
        match = false;
      }
    }

    if ((pattern->flags & SUFFIX_MATCH) && match) {
      DEBUG_PRINT("new_path_check() - pattern suffix: %s", pattern->suffix);
      if (is_suffix_match(pattern->suffix, buf->value + PATH_MAX*2, NAME_MAX + offset->second_name - 1)) {
        match = true;
      } else {
        match = false;
      }
    }
  }

  return match;
}

static __noinline bool head_path_check(struct path_pattern *pattern, struct buffer *buf, struct buffer_offset *offset) {

  DEBUG_PRINT("head_path_check() - pattern flags: 0x%x", pattern->flags);

  bool match = true;
  if (pattern->flags & GREEDY_MATCH || pattern->flags & PRECISE_MATCH) {
    // precise match or greedy match for the globbing "**" with file path
    DEBUG_PRINT("head_path_check() - matching path");

    if (pattern->flags & PREFIX_MATCH) {
      DEBUG_PRINT("head_path_check() - pattern prefix: %s", pattern->prefix);
      if (is_prefix_match(pattern->prefix, buf->value)) {
        match = true;
      } else {
        match = false;
      }
    }

    if ((pattern->flags & SUFFIX_MATCH) && match) {
      DEBUG_PRINT("head_path_check() - pattern suffix: %s", pattern->suffix);
      if (is_suffix_match(pattern->suffix, buf->value, offset->first_path - 2)) {
        match = true;
      } else {
        match = false;
      }
    }
  } else {
    // non-greedy match for the globbing "*" with file name
    DEBUG_PRINT("head_path_check() - matching name");

    if (pattern->flags & PREFIX_MATCH) {
      DEBUG_PRINT("head_path_check() - pattern prefix: %s", pattern->prefix);
      if (is_prefix_match(pattern->prefix, &(buf->value[PATH_MAX * 2]))) {
        match = true;
      } else {
        match = false;
      }
    }

    if ((pattern->flags & SUFFIX_MATCH) && match) {
      DEBUG_PRINT("head_path_check() - pattern suffix: %s", pattern->suffix);
      if (is_suffix_match(pattern->suffix, buf->value + PATH_MAX*2, offset->first_name - 1)) {
        match = true;
      } else {
        match = false;
      }
    }
  }

  return match;
}

#endif