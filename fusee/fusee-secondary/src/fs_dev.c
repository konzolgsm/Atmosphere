#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <sys/iosupport.h>
#include <sys/param.h>
#include <unistd.h>

#include "lib/fatfs/ff.h"

#include "fs_dev.h"

/* Quite a bit of code comes from https://github.com/switchbrew/libnx/blob/master/nx/source/runtime/devices/fs_dev.c */

static int fsdev_convert_rc(struct _reent *r, FRESULT rc);
static void fsdev_filinfo_to_st(struct stat *st, const FILINFO *info);

static int       fsdev_open(struct _reent *r, void *fileStruct, const char *path, int flags, int mode);
static int       fsdev_close(struct _reent *r, void *fd);
static ssize_t   fsdev_write(struct _reent *r, void *fd, const char *ptr, size_t len);
static ssize_t   fsdev_read(struct _reent *r, void *fd, char *ptr, size_t len);
static off_t     fsdev_seek(struct _reent *r, void *fd, off_t pos, int dir);
static int       fsdev_fstat(struct _reent *r, void *fd, struct stat *st);
static int       fsdev_stat(struct _reent *r, const char *file, struct stat *st);
static int       fsdev_link(struct _reent *r, const char *existing, const char  *newLink);
static int       fsdev_unlink(struct _reent *r, const char *name);
static int       fsdev_chdir(struct _reent *r, const char *name);
static int       fsdev_rename(struct _reent *r, const char *oldName, const char *newName);
static int       fsdev_mkdir(struct _reent *r, const char *path, int mode);
static DIR_ITER* fsdev_diropen(struct _reent *r, DIR_ITER *dirState, const char *path);
static int       fsdev_dirreset(struct _reent *r, DIR_ITER *dirState);
static int       fsdev_dirnext(struct _reent *r, DIR_ITER *dirState, char *filename, struct stat *filestat);
static int       fsdev_dirclose(struct _reent *r, DIR_ITER *dirState);
static int       fsdev_statvfs(struct _reent *r, const char *path, struct statvfs *buf);
static int       fsdev_ftruncate(struct _reent *r, void *fd, off_t len);
static int       fsdev_fsync(struct _reent *r, void *fd);
static int       fsdev_chmod(struct _reent *r, const char *path, mode_t mode);
static int       fsdev_fchmod(struct _reent *r, void *fd, mode_t mode);
static int       fsdev_rmdir(struct _reent *r, const char *name);

static devoptab_t g_fsdev_devoptab = {
  .structSize   = sizeof(FIL),
  .open_r       = fsdev_open,
  .close_r      = fsdev_close,
  .write_r      = fsdev_write,
  .read_r       = fsdev_read,
  .seek_r       = fsdev_seek,
  .fstat_r      = fsdev_fstat,
  .stat_r       = fsdev_stat,
  .link_r       = fsdev_link,
  .unlink_r     = fsdev_unlink,
  .chdir_r      = fsdev_chdir,
  .rename_r     = fsdev_rename,
  .mkdir_r      = fsdev_mkdir,
  .dirStateSize = sizeof(DIR),
  .diropen_r    = fsdev_diropen,
  .dirreset_r   = fsdev_dirreset,
  .dirnext_r    = fsdev_dirnext,
  .dirclose_r   = fsdev_dirclose,
  .statvfs_r    = fsdev_statvfs,
  .ftruncate_r  = fsdev_ftruncate,
  .fsync_r      = fsdev_fsync,
  .deviceData   = NULL,
  .chmod_r      = fsdev_chmod,
  .fchmod_r     = fsdev_fchmod,
  .rmdir_r      = fsdev_rmdir,
};

typedef struct fsdev_fsdevice_t {
    char name[32+1];
    bool setup;
    devoptab_t devoptab;
    FATFS fatfs;
} fsdev_fsdevice_t;

static fsdev_fsdevice_t g_devices[10] = { 0 };

/* Restrictions: 10 drives max, and **update FF_VOLUME_STRS accordingly** */
int fsdev_mount_device(const char *name) {
    if (name[0] == '\0' || strlen(name) > 32) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (FindDevice(name) != -1) {
        errno = EEXIST; /* Device already exists */
        return -1;
    }

    /* Find a free slot and use it */
    for (size_t i = 0; i < 10; i++) {
        if (!g_devices[i].setup) {
            fsdev_fsdevice_t *device = &g_devices[i];
            FRESULT rc;

            rc = f_mount(&device->fatfs, name, 1);

            if (rc != FR_OK) {
                return fsdev_convert_rc(NULL, rc);
            }

            strcpy(device->name, name);

            memcpy(&device->devoptab, &g_fsdev_devoptab, sizeof(devoptab_t));
            device->devoptab.name = device->name;
            device->devoptab.deviceData = device;

            if (AddDevice(&device->devoptab) == -1) {
                errno = ENOMEM;
                return -1;
            } else {
                device->setup = true;
                return 0;
            }
        }
    }

    errno = ENOMEM;
    return -1;
}

int fsdev_set_default_device(const char *name) {
#if FF_VOLUMES < 2
    return 0;
#else

    int ret;
    int devid = FindDevice(name);

    if (devid == -1) {
        errno = ENOENT;
        return -1;
    }

    ret = fsdev_convert_rc(NULL, f_chdrive(name));

    if (ret == 0) {
        setDefaultDevice(devid);
    }

    return ret;
#endif
}

int fsdev_unmount_device(const char *name) {
    int ret;
    int devid = FindDevice(name);

    if (devid == -1) {
        errno = ENOENT;
        return -1;
    }

    ret = fsdev_convert_rc(NULL, f_unmount(name));

    if (ret == 0) {
        fsdev_fsdevice_t *device = (fsdev_fsdevice_t *)(GetDeviceOpTab(name)->deviceData);
        RemoveDevice(name);
        memset(device, 0, sizeof(fsdev_fsdevice_t));
    }

    return ret;
}

int fsdev_mount_all(void) {
    static const char* const volid[] = { FF_VOLUME_STRS };

    for (size_t i = 0; i < sizeof(volid)/sizeof(volid[0]); i++) {
        int ret = fsdev_mount_device(volid[i]);
        if (ret != 0) {
            return ret;
        }
    }

    return 0;
}

int fsdev_unmount_all(void) {
    static const char* const volid[] = { FF_VOLUME_STRS };

    for (size_t i = 0; i < sizeof(volid)/sizeof(volid[0]); i++) {
        int ret = fsdev_unmount_device(volid[i]);
        if (ret != 0) {
            return ret;
        }
    }

    return 0;
}

/* Adapted from https://github.com/benemorius/openOBC-devboard/blob/bf0a4a33e22d24e7c299f921d185da27377310e0/lib/fatfs/FatFS.cpp#L173 */
static int fsdev_convert_rc(struct _reent *r, FRESULT rc) {
    int errornumber;
    switch(rc) {
        case FR_OK:                     /* (0) Succeeded */
            errornumber = 0;
            break;
        case FR_DISK_ERR:               /* (1) A hard error occurred in the low level disk I/O layer */
            errornumber = EIO;
            break;
        case FR_INT_ERR:                /* (2) Assertion failed */
            errornumber = EINVAL;
            break;
        case FR_NOT_READY:              /* (3) The physical drive cannot work */
            errornumber = EIO;
            break;
        case FR_NO_FILE:                /* (4) Could not find the file */
            errornumber = ENOENT;
            break;
        case FR_NO_PATH:                /* (5) Could not find the path */
            errornumber = ENOENT;
            break;
        case FR_INVALID_NAME:           /* (6) The path name format is invalid */
            errornumber = EINVAL;
            break;
        case FR_DENIED:                 /* (7) Access denied due to prohibited access or directory full */
            errornumber = EACCES;
            break;
        case FR_EXIST:                  /* (8) Access denied due to prohibited access */
            errornumber = EEXIST;
            break;
        case FR_INVALID_OBJECT:         /* (9) The file/directory object is invalid */
            errornumber = EFAULT;
            break;
        case FR_WRITE_PROTECTED:        /* (10) The physical drive is write protected */
            errornumber = EROFS;
            break;
        case FR_INVALID_DRIVE:          /* (11) The logical drive number is invalid */
            errornumber = ENODEV;
            break;
        case FR_NOT_ENABLED:            /* (12) The volume has no work area */
            errornumber = ENOEXEC;
            break;
        case FR_NO_FILESYSTEM:          /* (13) There is no valid FAT volume */
            errornumber = ENFILE;
            break;
        case FR_MKFS_ABORTED:           /* (14) The f_mkfs() aborted due to any parameter error */
            errornumber = ENOEXEC;
            break;
        case FR_TIMEOUT:                /* (15) Could not get a grant to access the volume within defined period */
            errornumber = EAGAIN;
            break;
        case FR_LOCKED:                 /* (16) The operation is rejected according to the file sharing policy */
            errornumber = EBUSY;
            break;
        case FR_NOT_ENOUGH_CORE:        /* (17) LFN working buffer could not be allocated */
            errornumber = ENOMEM;
            break;
        case FR_TOO_MANY_OPEN_FILES:    /* (18) Number of open files > _FS_SHARE */
            errornumber = EMFILE;
            break;
        case FR_INVALID_PARAMETER:      /* (19) Given parameter is invalid */
            errornumber = EINVAL;
            break;
        default:
            errornumber = EPERM;
            break;
    }

    if (r != NULL) {
        r->_errno = errornumber;
    } else {
        errno = errornumber;
    }
    return errornumber == 0 ? 0 : -1;
}

static void fsdev_filinfo_to_st(struct stat *st, const FILINFO *info) {
    /* Date of last modification */
    struct tm date;
    memset(st, 0, sizeof(struct stat));
    date.tm_mday = info->fdate & 31;
    date.tm_mon  = ((info->fdate >> 5) & 15) - 1;
    date.tm_year = ((info->fdate >> 9) & 127) - 1980 + 1900;

    date.tm_sec  = (info->ftime << 1) & 63;
    date.tm_min  = (info->ftime >> 5) & 63;
    date.tm_hour = (info->ftime >> 11) & 31;

    st->st_atime = st->st_mtime = st->st_ctime = mktime(&date);
    st->st_size = (off_t)info->fsize;
    st->st_nlink = 1;

    if (info->fattrib & AM_DIR) {
        st->st_mode = S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO;
    } else {
        st->st_mode = S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
    }
}

static int fsdev_open(struct _reent *r, void *fileStruct, const char *path, int flags, int mode) {
    (void)mode;
    FIL *f = (FIL *)fileStruct;

    BYTE ff_flags = 0;
    static const struct {
        int posix_flag;
        BYTE ff_flag;
    } flag_mappings[] = {
        { O_APPEND, FA_OPEN_APPEND     },
        { O_CREAT , FA_OPEN_ALWAYS     },
        { O_EXCL  , FA_CREATE_NEW      },
        { O_TRUNC , FA_CREATE_ALWAYS   },
        { O_RDONLY, FA_READ            },
        { O_WRONLY, FA_WRITE           },
        { O_RDWR  , FA_READ | FA_WRITE },
    };

    if ((flags & O_RDONLY & O_WRONLY) || ((flags & O_RDWR) & (O_RDONLY | O_WRONLY))) {
        r->_errno = EINVAL;
        return -1;
    }
    if (flags & O_RDONLY & O_APPEND) {
        r->_errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < sizeof(flag_mappings)/sizeof(flag_mappings[0]); i++) {
        if (flags & flag_mappings[i].posix_flag) {
            ff_flags |= flag_mappings[i].ff_flag;
        }
    }
    /* Normalize O_CREAT|O_TRUNC */
    if ((ff_flags & (FA_CREATE_ALWAYS | FA_OPEN_ALWAYS)) == (FA_CREATE_ALWAYS | FA_OPEN_ALWAYS)) {
        ff_flags &= ~FA_OPEN_ALWAYS;
    }

    return fsdev_convert_rc(r, f_open(f, path, ff_flags));
}

static int fsdev_close(struct _reent *r, void *fd) {
    FIL *f = (FIL *)fd;
    return fsdev_convert_rc(r, f_close(f));
}

static ssize_t fsdev_write(struct _reent *r, void *fd, const char *ptr, size_t len) {
    FIL *f = (FIL *)fd;
    UINT wr;

    int ret = fsdev_convert_rc(r, f_write(f, ptr, (UINT)len, &wr));
    return ret == -1 ? -1 : (ssize_t)wr;
}

static ssize_t fsdev_read(struct _reent *r, void *fd, char *ptr, size_t len) {
    FIL *f = (FIL *)fd;
    UINT rd;

    int ret = fsdev_convert_rc(r, f_read(f, ptr, (UINT)len, &rd));
    return ret == -1 ? -1 : (ssize_t)rd;
}

static off_t fsdev_seek(struct _reent *r, void *fd, off_t pos, int whence) {
    FIL *f = (FIL *)f;
    FSIZE_t off;
    int ret;

    switch (whence) {
        case SEEK_SET:
            off = 0;
            break;
        case SEEK_CUR:
            off = f_tell(f);
            break;
        case SEEK_END:
            off = f_size(f);
            break;
        default:
            r->_errno = EINVAL;
            return -1;
    }

    if(pos < 0 && off < -pos) {
        /* don't allow seek to before the beginning of the file */
        r->_errno = EINVAL;
        return -1;
    }

    ret = fsdev_convert_rc(r, f_lseek(f, off + (FSIZE_t)pos));
    return ret == -1 ? -1 : (off_t)(off + (FSIZE_t)pos);
}

static int fsdev_fstat(struct _reent *r, void *fd, struct stat *st) {
    (void)fd;
    (void)st;

    r->_errno = ENOSYS;
    return -1;
}

static int fsdev_stat(struct _reent *r, const char *file, struct stat *st) {
    FILINFO info;
    FRESULT rc = f_stat(file, &info);

    if (rc == FR_OK) {
        fsdev_filinfo_to_st(st, &info);
        return 0;
    }

    return fsdev_convert_rc(r, rc);
}

static int fsdev_link(struct _reent *r, const char *existing, const char *newLink) {
    (void)existing;
    (void)newLink;
    r->_errno = ENOSYS;
    return -1;
}

static int fsdev_unlink(struct _reent *r, const char *name) {
    return fsdev_convert_rc(r, f_unlink(name));
}

static int fsdev_chdir(struct _reent *r, const char *name) {
    return fsdev_convert_rc(r, f_chdir(name));
}

static int fsdev_rename(struct _reent *r, const char *oldName, const char *newName) {
    return fsdev_convert_rc(r, f_rename(oldName, newName));
}

static int fsdev_mkdir(struct _reent *r, const char *path, int mode) {
    (void)mode;
    return fsdev_convert_rc(r, f_mkdir(path));
}

static DIR_ITER* fsdev_diropen(struct _reent *r, DIR_ITER *dirState, const char *path) {
    DIR *d = (DIR *)(dirState->dirStruct);
    return fsdev_convert_rc(r, f_opendir(d, path)) == -1 ? NULL : dirState;
}

static int fsdev_dirreset(struct _reent *r, DIR_ITER *dirState) {
    (void)dirState;
    r->_errno = ENOSYS;
    return -1;
}

static int fsdev_dirnext(struct _reent *r, DIR_ITER *dirState, char *filename, struct stat *filestat) {
    FILINFO info;
    DIR *d = (DIR *)(dirState->dirStruct);
    FRESULT rc = f_readdir(d, &info);

    if (rc == FR_OK) {
        fsdev_filinfo_to_st(filestat, &info);

        if(info.fname[0] == '\0') {
            /* End of directory */
            r->_errno = ENOENT;
            return -1;
        }

        strcpy(filename, info.fname);
        return 0;
    } else {
        return fsdev_convert_rc(r, rc);
    }
}

static int fsdev_dirclose(struct _reent *r, DIR_ITER *dirState) {
    DIR *d = (DIR *)(dirState->dirStruct);
    return fsdev_convert_rc(r, f_closedir(d));
}

static int fsdev_statvfs(struct _reent *r, const char *path, struct statvfs *buf) {
    (void)path;

    DWORD nclust;
    FATFS *fatfs;
    FRESULT rc;

    rc = f_getfree(path, &nclust, &fatfs);

    if (rc == FR_OK) {
#if FF_MAX_SS != FF_MIN_SS
        buf->f_bsize   = fatfs->ssize;
#else
        buf->f_bsize   = FF_MAX_SS;
#endif
        buf->f_frsize  = buf->f_bsize;
        buf->f_blocks  = (fatfs->n_fatent - 2) * fatfs->csize;
        buf->f_bfree   = nclust * fatfs->csize;
        buf->f_bavail  = buf->f_bfree;
        buf->f_files   = 0;
        buf->f_ffree   = 0;
        buf->f_favail  = 0;
        buf->f_fsid    = 0;
        buf->f_flag    = ST_NOSUID;
        buf->f_namemax = FF_LFN_BUF;

        return 0;
    } else {
        return fsdev_convert_rc(r, rc);
    }
}

static int fsdev_ftruncate(struct _reent *r, void *fd, off_t len) {
    FIL *f = (FIL *)fd;
    FSIZE_t off = f_tell(f); /* No need to validate this */
    FRESULT rc = f_lseek(f, (FSIZE_t)len);

    off = off > (FSIZE_t)len ? (FSIZE_t)len : off;
    if (rc == FR_OK) {
        rc = f_truncate(f);
        /* Ignore errors while attempting restore the position */
        f_lseek(f, off);
        return fsdev_convert_rc(r, rc);
    } else {
        return fsdev_convert_rc(r, rc);
    }
}

static int fsdev_fsync(struct _reent *r, void *fd) {
    FIL *f = (FIL *)fd;
    return fsdev_convert_rc(r, f_sync(f));
}

static int fsdev_chmod(struct _reent *r, const char *path, mode_t mode) {
    (void)path;
    (void)mode;
    r->_errno = ENOSYS;
    return -1;
}

static int fsdev_fchmod(struct _reent *r, void *fd, mode_t mode) {
    (void)fd;
    (void)mode;
    r->_errno = ENOSYS;
    return -1;
}

static int fsdev_rmdir(struct _reent *r, const char *name) {
    return fsdev_convert_rc(r, f_unlink(name));
}
