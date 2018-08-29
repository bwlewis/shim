/*
 * Copyright (c) 2005 Bryan W. Lewis <blewis@illposed.net>
 *
 * This program (shim) is free software; you can redistribute 
 * it and/or modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * Pvshm is an experimental overlay file system that intercepts memory-mapped 
 * operations and replaces them with file read/write calls to another file 
 * system. Pvshm is designed to provide basic mmap support over file systems
 * that don't natively support mmap. Basic read and write operations are
 * simply passed-through to the backing file system.
 *
 * OK, I know what you're about to ask: why not just use fuse? The fuse
 * (experimental) writable mmap code is not easy to follow, and is focused
 * on a very general-purpose, cache-consistent model, and I'm not sure if it
 * is still even being developed. We intentionally dispense with consistency 
 * leaving that to the applications anyway, and try to keep things very simple.
 */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/statfs.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/backing-dev.h>
#include <linux/pagevec.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <linux/mm.h>
#include <linux/writeback.h>
#include <linux/syscalls.h>
#include <linux/mpage.h>
#include <linux/pagemap.h>
#include <linux/vmalloc.h>
#include <linux/uio.h>

#define PVSHM_MAGIC	0x55566655
#define list_to_page(head) (list_entry((head)->prev, struct page, lru))

int verbose = 1;
unsigned int read_ahead = 1024;

// Used by writepages
ssize_t
write_block (struct file *file, char __user * buf, int m, loff_t offset)
{
  ssize_t w;
  mm_segment_t old_fs;
  old_fs = get_fs ();
  set_fs (get_ds ());
  w = kernel_write (file, buf, m * PAGE_SIZE, &offset);
  set_fs (old_fs);
  return w;
}

/* Superblock and file inode operations */
const struct inode_operations shim_dir_inode_operations;
const struct inode_operations shim_file_inode_operations;
struct dentry *shim_get_sb (struct file_system_type *fs_type,
                            int flags, const char *dev_name, void *data);
struct inode *shim_iget (struct super_block *sp, unsigned long ino);

/* Address space operations */
static int shim_writepage (struct page *page, struct writeback_control *wbc);
static int shim_readpage (struct file *file, struct page *page);
static int shim_readpages (struct file *file, struct address_space *mapping,
                           struct list_head *page_list, unsigned nr_pages);
static int shim_writepages (struct address_space *mapping,
                            struct writeback_control *wbc);
static int shim_set_page_dirty_nobuffers (struct page *page);
static int shim_releasepage (struct page *page, gfp_t gfp_flags);
static void shim_invalidatepage (struct page *page, unsigned int offset,
                                 unsigned int length);

/* File operations */
static int shim_file_mmap (struct file *, struct vm_area_struct *);
static int shim_sync_file (struct file *, loff_t, loff_t, int);
ssize_t shim_write (struct file *, const char __user *, size_t, loff_t *);
ssize_t shim_file_read_iter (struct kiocb *, struct iov_iter *);

/*
 * path: The target file full path
 * file: The target file stream 
 * Stored in each shim inode private field
 */
typedef struct
{
  char *path;
  loff_t max_size;
  struct file *file;
} shim_target;


struct super_operations shim_sops = {
  .statfs = simple_statfs,
};

const struct address_space_operations shim_aops = {
  .readpage = shim_readpage,
  .readpages = shim_readpages,
  .writepage = shim_writepage,
  .writepages = shim_writepages,
  .set_page_dirty = shim_set_page_dirty_nobuffers,
  .releasepage = shim_releasepage,
  .invalidatepage = shim_invalidatepage,
};

const struct file_operations shim_file_operations = {
  .mmap = shim_file_mmap,
  .fsync = shim_sync_file,
  .llseek = generic_file_llseek,
  .read_iter = shim_file_read_iter,
  .open = simple_open,
  .write = shim_write,
  .llseek = generic_file_llseek,
  .splice_read = generic_file_splice_read,
};

const struct inode_operations shim_file_inode_operations = {
  .getattr = simple_getattr,
};


/* Inode operations */
struct inode *
shim_iget (struct super_block *sb, unsigned long ino)
{
  struct inode *inode;
  inode = iget_locked (sb, ino);
  if (!inode)
    return ERR_PTR (-ENOMEM);
  unlock_new_inode (inode);
  return inode;
}


/* File operations */

/* Pass normal read operations through to the backing file.
 *
 * Use lseek and read with a NULL buffer to 'reverse msync' the page mapping
 * over the pages covering the byte range following the lseek and including
 * bytes specified in the (fake) read operation.
 */
ssize_t
shim_file_read_iter (struct kiocb * iocb, struct iov_iter * iter)
{
  struct file *file;
  struct address_space *mapping;
  struct inode *inode;
  pgoff_t pstart, pend;
  struct pagevec pvec;
  int i;
  shim_target *pvmd;
  size_t count = iov_iter_count (iter);
  if (verbose)
    printk (KERN_DEBUG "shim_file_read_iter %d\n", (int) count);

  file = iocb->ki_filp;
  mapping = file->f_mapping;
  inode = mapping->host;
  pvmd = (shim_target *) inode->i_private;
  if (!pvmd)
    {
      printk (KERN_WARNING "shim_file_read_iter missing target file\n");
      return -1;
    }

  if (iter->iov->iov_base)
    return generic_file_read_iter (iocb, iter);

  pstart = iocb->ki_pos >> PAGE_SHIFT;
  pend = ((iocb->ki_pos + count + PAGE_SIZE - 1) >> PAGE_SHIFT);
// XXX add page index range check

  pagevec_init (&pvec);
  pagevec_lookup_range (&pvec, mapping, &pstart, pend);
  for (i = 0; i < pagevec_count (&pvec); i++)
    {
      struct page *page = pvec.pages[i];
      int lock_failed;
      pgoff_t index;
      lock_failed = trylock_page (page);
      index = page->index;
      ClearPageUptodate (page);
      if (verbose)
        printk (KERN_DEBUG "reverse sync: page->index=%d %s %s\n",
                (int) page->index,
                PageUptodate (page) ? "Uptodate" :
                "Not Uptodate", PageLocked (page) ? "Locked" : "Unlocked");
      if (PageLocked (page))
        unlock_page (page);
      shim_readpage (file, page);
    }
  pagevec_release (&pvec);
  return 0;
}

/* shim_write
 *
 * The shim_write function passes usual write operations to the backing file.
 * XXX
 * XXX This may not make any sense. Instead:
 * 1. release any open target file descriptor
 * 2. pass write through to target file
 * 3. re-open target file descriptor (as in symlink)
  
 */
ssize_t
shim_write (struct file * filp, const char __user * buf, size_t len,
            loff_t * skip)
{
  mm_segment_t old_fs;
  ssize_t ret = -EBADF;
  struct inode *inode = filp->f_mapping->host;
  shim_target *pvmd = (shim_target *) inode->i_private;
  if (!pvmd)
    goto out;
  if (buf)
    {
      old_fs = get_fs ();
      set_fs (KERNEL_DS);
      ret = kernel_write (pvmd->file, (char __user *) buf, len, skip);
      set_fs (old_fs);
      if (verbose)
        printk (KERN_INFO "shim_write to backing file %s\n", pvmd->path);
    }
out:
  return ret;
}


static int
shim_file_mmap (struct file *f, struct vm_area_struct *v)
{
  int ret = -EBADF;
  shim_target *pvmd = (shim_target *) f->f_mapping->host->i_private;
  if (!pvmd)
    goto out;
  if (verbose)
    printk (KERN_INFO "shim_file_mmap %s\n", pvmd->path);
  ret = generic_file_mmap (f, v);
out:
  return ret;
}

static int
shim_sync_file (struct file *f, loff_t start, loff_t end, int k)
{
  shim_target *pv_tgt;
  int j = -EBADF;
  struct inode *inode = f->f_mapping->host;

  pv_tgt = (shim_target *) inode->i_private;
  if (!pv_tgt)
    goto out;
  if (verbose)
    printk (KERN_INFO "shim_sync_file %s k=%d\n", pv_tgt->path, k);
  j = filemap_write_and_wait (f->f_mapping);
out:
  return j;
}


/* inode operations */
struct inode *
shim_get_inode (struct super_block *sb, umode_t mode, dev_t dev)
{
  struct inode *inode = new_inode (sb);
  if (inode)
    {
      inode->i_mode = mode;
      inode->i_uid = current_uid ();
      inode->i_gid = current_gid ();
      inode->i_blocks = 0;
      inode->i_mapping->a_ops = &shim_aops;
      inode->i_atime = inode->i_mtime = inode->i_ctime =
        current_kernel_time ();
      switch (mode & S_IFMT)
        {
        default:
          init_special_inode (inode, mode, dev);
          break;
        case S_IFREG:
          inode->i_op = &shim_file_inode_operations;
          inode->i_fop = &shim_file_operations;
          break;
        case S_IFDIR:
          inode->i_op = &shim_dir_inode_operations;
          inode->i_fop = &simple_dir_operations;
          inc_nlink (inode);
          break;
        case S_IFLNK:
          inode->i_op = &shim_file_inode_operations;
          inode->i_fop = &shim_file_operations;
          break;
        }
    }
  return inode;
}

/* Patterned after ramfs */
static int
shim_mknod (struct inode *dir, struct dentry *dentry, umode_t mode, dev_t dev)
{
  int error = -ENOSPC;
  struct inode *inode = shim_get_inode (dir->i_sb, mode, dev);
  if (verbose)
    printk (KERN_INFO "shim_mknod d_name=%s\n", dentry->d_name.name);

  if (inode)
    {
      if (dir->i_mode & S_ISGID)
        {
          inode->i_gid = dir->i_gid;
          if (S_ISDIR (mode))
            inode->i_mode |= S_ISGID;
        }
      d_instantiate (dentry, inode);
      dget (dentry);
      error = 0;
      dir->i_mtime = dir->i_ctime = current_kernel_time ();
    }
  return error;
}

static int
shim_mkdir (struct inode *dir, struct dentry *dentry, umode_t mode)
{
  int retval = shim_mknod (dir, dentry, mode | S_IFDIR, 0);
  if (!retval)
    inc_nlink (dir);
  return retval;
}

static int
shim_create (struct inode *dir, struct dentry *dentry, umode_t mode,
             bool exclusive)
{
  return shim_mknod (dir, dentry, mode | S_IFREG, 0);
}

/* shim_unlink 
 * Remove the inode, de-allocate housekeeping storage for its target,
 * close the open file descriptor to the target. 
 */
static int
shim_unlink (struct inode *dir, struct dentry *d)
{
  struct inode *ino = d->d_inode;
  shim_target *pvmd = (shim_target *) ino->i_private;
  if (pvmd)
    {
      if (verbose)
        printk (KERN_INFO "shim_unlink %s\n", pvmd->path);
      if (pvmd->file)
        filp_close (pvmd->file, current->files);
      kfree (pvmd->path);
      kfree (pvmd);
    }
  return simple_unlink (dir, d);
}

/* Create a shim entry and set up a mapping between the shim file 
 * and the target file specified by symname.
 * 
 * Open a r/w file stream to the target.
 */
static int
shim_symlink (struct inode *dir, struct dentry *dentry, const char *symname)
{
  struct inode *inode;
  int error = -ENOSPC;
  ino_t j = iunique (dir->i_sb, 0);
  shim_target *pvmd = (shim_target *) kmalloc (sizeof (shim_target), 0);

  struct kstat stat;
  mm_segment_t old_fs = get_fs ();
  set_fs (KERNEL_DS);
  error = vfs_stat ((char *) symname, &stat);
  set_fs (old_fs);
  if (error)
    {
      if (verbose)
        printk (KERN_WARNING "shim_symlink can't stat target file\n");
      kfree (pvmd);
      goto end;
    }
  if ((stat.mode & S_IFMT) == S_IFDIR)
    {
      if (verbose)
        printk (KERN_WARNING
                "shim_symlink only works with files (not directories)\n");
      kfree (pvmd);
      goto end;
    }
  pvmd->max_size = stat.size;

  inode = shim_iget (dir->i_sb, j);
  inode->i_mode = stat.mode;
  inode->i_uid = stat.uid;
  inode->i_gid = stat.gid;
  inode->i_fop = &shim_file_operations;
  inode->i_mapping->a_ops = &shim_aops;
  if (verbose)
    printk (KERN_INFO "shim_symlink d_name=%s, symname=%s\n",
            dentry->d_name.name, symname);
  if (inode)
    {
      int l = strlen (symname) + 1;
/* We don't do this: page_symlink(inode, symname, l);
 * The standard approach of putting the symlink name in page 0 does not 
 * work in this case since we use all pages in the mapping.  We allocate 
 * space for the file name and store it in the inode private field.
 */
      error = 0;
      pvmd->path = (char *) kmalloc (l, 0);
      memcpy (pvmd->path, symname, l);
      pvmd->file = filp_open (symname, O_RDWR | O_LARGEFILE, 0);
      if (!pvmd->file)
        {
          error = -EBADF;
          if (verbose)
            printk (KERN_INFO "shim_symlink symname=%s unable to open\n",
                    symname);
          kfree(pvmd->path);
          kfree (pvmd);
          pvmd = NULL;
        }

      inode->i_private = pvmd;
      if (!error)
        {
          if (dir->i_mode & S_ISGID)
            inode->i_gid = dir->i_gid;
          inode->i_size = stat.size;
          d_instantiate (dentry, inode);
          dget (dentry);
          dir->i_mtime = dir->i_ctime = current_kernel_time ();
        }
      else
        iput (inode);
    }

end:
  return error;
}

const struct inode_operations shim_dir_inode_operations = {
  .create = shim_create,
  .link = simple_link,
  .unlink = shim_unlink,
  .symlink = shim_symlink,
  .mkdir = shim_mkdir,
  .rmdir = simple_rmdir,
  .mknod = shim_mknod,
  .rename = simple_rename,
  .lookup = simple_lookup,
  .setattr = simple_setattr,
};

static struct file_system_type shim_fs_type = {
  .name = "shim",
  .mount = shim_get_sb,
  .kill_sb = kill_litter_super,
  .owner = THIS_MODULE,
};

static int
shim_fill_super (struct super_block *sb, void *data, int silent)
{
  static struct inode *shim_root_inode;
  struct dentry *root;
  int j;

  if (verbose)
    printk (KERN_INFO "shim_fill_super\n");
  sb->s_maxbytes = MAX_LFS_FILESIZE;
  sb->s_blocksize = PAGE_SIZE;
  sb->s_blocksize_bits = PAGE_SHIFT;
  sb->s_magic = PVSHM_MAGIC;
  sb->s_op = &shim_sops;
  sb->s_type = &shim_fs_type;
  sb->s_time_gran = 1;
  j = super_setup_bdi (sb);
  if (j)
    return j;
  sb->s_bdi->ra_pages = read_ahead;
  if (verbose)
    printk (KERN_INFO "sb->s_bdi->ra_pages = %u\n", read_ahead);
  shim_root_inode = shim_get_inode (sb, S_IFDIR | 0755, 0);
  if (!shim_root_inode)
    return -ENOMEM;

  root = d_make_root (shim_root_inode);
  if (!root)
    {
      iput (shim_root_inode);
      return -ENOMEM;
    }
  sb->s_root = root;
  return 0;
}

struct dentry *
shim_get_sb (struct file_system_type *fs_type,
             int flags, const char *dev_name, void *data)
{
  return mount_nodev (fs_type, flags, data, shim_fill_super);
}

static int
shim_set_page_dirty_nobuffers (struct page *page)
{
  int j = 0;
  j = __set_page_dirty_nobuffers (page);
  if (verbose)
    printk (KERN_INFO "shim_spdirty_nb: %d [%s] [%s] [%s] [%s]\n",
            (int) page->index,
            PageUptodate (page) ? "Uptodate" : "Not Uptodate",
            PageDirty (page) ? "Dirty" : "Not Dirty",
            PageWriteback (page) ? "PWrbk Set" : "PWrbk Cleared",
            PageLocked (page) ? "Locked" : "Unlocked");
  return j;
}

/* Release the private state associated with a page */
static int
shim_releasepage (struct page *page, gfp_t gfp_flags)
{
  if (verbose)
    printk (KERN_INFO "shim_releasepage private = %d\n", PagePrivate (page));
  return 0;
}

static void
shim_invalidatepage (struct page *page, unsigned int offset,
                     unsigned int length)
{
  if (verbose)
    printk (KERN_INFO "shim_invalidatepage private = %d\n",
            PagePrivate (page));
  shim_releasepage (page, 0);
}

static int
shim_writepage (struct page *page, struct writeback_control *wbc)
{
  ssize_t j;
  loff_t offset;
  mm_segment_t old_fs;
  struct inode *inode;
  void *page_addr;
  shim_target *pvmd;

  inode = page->mapping->host;
  pvmd = (shim_target *) inode->i_private;
  offset = page->index << PAGE_SHIFT;
  j = 1;
  test_set_page_writeback (page);
  if (pvmd->file)
    {
/* NB. We unfortunately can't use vfs_write inside an atomic section,
 * precluding the speedier page_addr = kmap_atomic (page, KM_USER0).
 */
      page_addr = kmap (page);
      old_fs = get_fs ();
      set_fs (get_ds ());
      j = kernel_write (pvmd->file, (char __user *) page_addr,
                        PAGE_SIZE, &offset);
      set_fs (old_fs);
      kunmap (page);
    }

  if (!PageUptodate (page))
    SetPageUptodate (page);
  if (PageError (page))
    ClearPageError (page);
  if (PageLocked (page))
    unlock_page (page);

  end_page_writeback (page);

  if (verbose)
    printk
      (KERN_INFO
       "shim_writepage: %d link=%s [%s] [%s] [%s] [%s] count %d nr_to_write %ld\n",
       (int) page->index, (char *) pvmd->path,
       PageUptodate (page) ? "Uptodate" : "Not Uptodate",
       PageDirty (page) ? "Dirty" : "Not Dirty",
       PagePrivate (page) ? "Private" : "Not Private",
       PageLocked (page) ? "Locked" : "Unlocked", page_count (page),
       wbc->nr_to_write);

  wbc->nr_to_write -= 1;
  if (wbc->sync_mode == WB_SYNC_NONE)
    invalidate_inode_pages2_range (inode->i_mapping,
                                   page->index, page->index);

  return 0;
}

/* This is an experimental routine that writes out blocks of contiguous
 * pages for efficiency. The largest block size is set, somewhat arbitrarily,
 * to be the same as read_ahead.
 */
static int
shim_writepages (struct address_space *mapping, struct writeback_control *wbc)
{
  pgoff_t index;
  pgoff_t end;
  loff_t offset;
  int n, j, m, start, k;
  struct page **p;
  void *buf;
  struct inode *inode = mapping->host;
  shim_target *pvmd = (shim_target *) inode->i_private;
  j = 0;
  start = 0;
  end = -1;
  p = kmalloc (sizeof (struct page *) * read_ahead, GFP_NOFS);
  if (wbc->range_cyclic)
    index = mapping->writeback_index;
  else
    {
      index = wbc->range_start >> PAGE_SHIFT;
      end = wbc->range_end >> PAGE_SHIFT;
    }
  while (index <= end)
    {
      n = find_get_pages_tag (mapping, &index, PAGECACHE_TAG_DIRTY,
                              read_ahead, p);
      if (n == 0)
        break;
      if (verbose)
        printk (KERN_INFO
                "shim_writepages ndirty=%d index=%ld nr_to_write=%ld\n", n,
                index, wbc->nr_to_write);
/* Search the array of dirty pages for contiguous blocks */
      if (n > 1)
        {
          start = 0;
          for (j = 0; j < (n - 1); ++j)
            {
              if (p[j + 1]->index != p[j]->index + 1)
                {
                  m = (int) (p[j]->index - p[start]->index + 1);
                  if (m > 1)
                    {
                      if (verbose)
                        printk (KERN_INFO
                                "shim_writepages start=%ld end=%ld\n",
                                p[start]->index, p[j]->index);
                      offset = p[start]->index << PAGE_SHIFT;
                      for (k = 0; k < m; ++k)
                        {
                          lock_page (p[start + k]);
                          clear_page_dirty_for_io (p[start + k]);
                        }
                      buf = vmap (&p[start], m, VM_MAP, PAGE_KERNEL);
                      if (!buf)
                        goto out;
                      write_block (pvmd->file, (char __user *) buf, m,
                                   offset);
                      vunmap (buf);
                      spin_lock_irq (&mapping->private_lock);
                      for (k = 0; k < m; ++k)
                        {
                          unlock_page (p[start + k]);
                          radix_tree_tag_clear (&mapping->i_pages,
                                                page_index (p[start + k]),
                                                PAGECACHE_TAG_DIRTY);
                        }
                      spin_unlock_irq (&mapping->private_lock);
                    }
                  start = j + 1;
                }
            }
        }
      m = (int) (p[j]->index - p[start]->index + 1);
      if (m < 2)
        goto out;
      if (verbose)
        printk (KERN_INFO "shim_writepages start=%ld end=%ld\n",
                p[start]->index, p[j]->index);
      offset = p[start]->index << PAGE_SHIFT;
      for (k = 0; k < m; ++k)
        {
          lock_page (p[start + k]);
          clear_page_dirty_for_io (p[start + k]);
        }
      buf = vmap (&p[start], m, VM_MAP, PAGE_KERNEL);
      if (!buf)
        goto out;
      write_block (pvmd->file, (char __user *) buf, m, offset);
      vunmap (buf);
      spin_lock_irq (&mapping->private_lock);
      for (k = 0; k < m; ++k)
        {
          unlock_page (p[start + k]);
          radix_tree_tag_clear (&mapping->i_pages,
                                page_index (p[start + k]),
                                PAGECACHE_TAG_DIRTY);
        }
      spin_unlock_irq (&mapping->private_lock);
      if (wbc->nr_to_write)
        wbc->nr_to_write -= m;
      if (wbc->sync_mode == WB_SYNC_NONE)
        {
// Called for memory cleansing
          invalidate_inode_pages2_range (inode->i_mapping,
                                         p[start]->index, p[j]->index);
        }
    }
out:
  kfree (p);
/* Write out any remaining pages */
  return generic_writepages (mapping, wbc);
}

static int
shim_readpages (struct file *file, struct address_space *mapping,
                struct list_head *pages, unsigned nr_pages)
{
  unsigned page_idx;
  void *page_addr;
  void *p;
  void *buf;
  mm_segment_t old_fs;
  loff_t offset;
  size_t res;
  struct page *pg = list_to_page (pages);
  struct inode *inode = file->f_mapping->host;
  shim_target *pvmd = (shim_target *) inode->i_private;
  unsigned int n = 0, m = 0;
  struct page **pg_array;

  if ((!pvmd) || (!pvmd->file))
    {
      printk (KERN_WARNING "shim_readpages no backing file\n");
      return -1;
    }
  pg_array = kmalloc (sizeof (pg) * nr_pages, GFP_NOFS);
  if (!pg_array)
    return -ENOMEM;
  if (verbose)
    printk (KERN_INFO "shim_readpages %d\n", (int) nr_pages);
  n = pg->index;
  offset = n << PAGE_SHIFT;
  list_for_each_entry_reverse (pg, pages, lru)
  {
    if (n == pg->index)
      {
        pg_array[m] = pg;
        ++m;
        ++n;
      }
    else
      break;
  }
/* m now contains the number of contiguous pages at the start of the list. */
  buf = vmap (pg_array, m, VM_MAP, PAGE_KERNEL);
  if (!buf)
    {
      kfree (pg_array);
      return -ENOMEM;
    }
  p = buf;
  if (verbose)
    printk (KERN_INFO "shim_readpages contiguous = %ld\n", (long) m);
/* Read the contiguous block at once */
  old_fs = get_fs ();
  set_fs (get_ds ());
  res = kernel_read (pvmd->file, (char __user *) buf, m * PAGE_SIZE, &offset);
  set_fs (old_fs);
  if (verbose)
    printk (KERN_INFO "shim_readpages bytes read = %ld\n", (long) res);
/* Copy buffer into pages and read any remaning pages after the block one
 * by one.
 */
  for (page_idx = 0; page_idx < nr_pages; page_idx++)
    {
      pg = list_to_page (pages);
      list_del (&pg->lru);
      if (!add_to_page_cache_lru (pg, mapping, pg->index, GFP_KERNEL))
        {
          if (page_idx < m)
            {
              page_addr = kmap (pg);
              if (verbose)
                printk
                  (KERN_INFO
                   "Copying to page addr %p index %ld from buffer addr %p\n",
                   page_addr, pg->index, p);
              copy_page (page_addr, p);
              p += PAGE_SIZE;
              kunmap (pg);
              SetPageUptodate (pg);
              if (PageLocked (pg))
                unlock_page (pg);
            }
          else
            mapping->a_ops->readpage (file, pg);
        }
      put_page (pg);
    }
  vunmap (buf);
  kfree (pg_array);
  return 0;
}

static int
shim_readpage (struct file *file, struct page *page)
{
  void *page_addr;
  loff_t offset;
  mm_segment_t old_fs;
  int j;
  struct inode *inode = file->f_mapping->host;
  shim_target *pvmd = (shim_target *) inode->i_private;
  if (verbose)
    printk (KERN_INFO "shim_readpage %d %s %ld [%s] [%s] [%s]\n",
            (int) page->index,
            (char *) pvmd->path,
            page->mapping->nrpages,
            PageUptodate (page) ? "Uptodate" : "Not Uptodate",
            PageDirty (page) ? "Dirty" : "Not Dirty",
            PageLocked (page) ? "Locked" : "Unlocked");
  page_addr = kmap (page);
  if (page_addr)
    {
      j = 0;
      offset = page->index << PAGE_SHIFT;
      if (verbose)
        printk (KERN_INFO "shim_readpage offset=%ld, page_addr=%p\n",
                (long) offset, page_addr);
      if (pvmd->file)
        {
          old_fs = get_fs ();
          set_fs (KERNEL_DS);
          j =
            kernel_read (pvmd->file, (char __user *) page_addr, PAGE_SIZE,
                         &offset);
          set_fs (old_fs);
        }
      if (verbose)
        printk (KERN_INFO "readpage %d bytes at index %d complete\n", j,
                (int) page->index);
/* XXX It may be that the backing file is not a multiple of the page size,
 * resulting in j < PAGE_SIZE. We should probably clear the remainder of
 * the page here, or prior to this with page_zero...
 */
      kunmap (page);
      SetPageUptodate (page);
    }
  if (PageLocked (page))
    unlock_page (page);
  return 0;
}


static int __init
init_shim_fs (void)
{
  int j;
  j = register_filesystem (&shim_fs_type);
  printk (KERN_INFO "shim module loaded\n");
  return j;
}

static void __exit
exit_shim_fs (void)
{
  printk (KERN_INFO "shim module unloaded.\n");
  unregister_filesystem (&shim_fs_type);
}

module_init (init_shim_fs);
module_exit (exit_shim_fs);

module_param (verbose, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC (verbose, " 1 -> verbose on (default=0)");
module_param (read_ahead, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC (read_ahead, " Nr. of pages to read ahead, (default=1024)");

MODULE_AUTHOR ("Bryan Wayne Lewis");
MODULE_LICENSE ("GPL");
