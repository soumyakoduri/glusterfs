#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <limits.h>
#include <alloca.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <glusterfs/api/glfs.h>
#include <glusterfs/api/glfs-handles.h>
int gfapi = 1;

#define LOG_ERR(func, ret) do { \
        if (ret != 0) {            \
                fprintf (stderr, "%s : returned error %d (%s)\n", \
                         func, ret, strerror (errno)); \
                goto out; \
        } else { \
                fprintf (stderr, "%s : returned %d\n", func, ret); \
        } \
        } while (0)

int
main (int argc, char *argv[])
{
        glfs_t    *fs = NULL;
        glfs_t    *fs2 = NULL;
        glfs_t    *fs_tmp = NULL;
        int        ret = 0, i;
        glfs_fd_t *fd = NULL;
        glfs_fd_t *fd2 = NULL;
        glfs_fd_t *fd_tmp = NULL;
        char       readbuf[32];
        char      *filename = "file_tmp";
        char      *writebuf = NULL;
        char      *vol_id  = NULL;
        int       cnt = 1;
        struct    callback_arg cbk;
        char      *logfile = NULL;
        char      *volname = NULL;

        cbk.glhandle = NULL;

        if (argc != 3) {
                fprintf (stderr, "Invalid argument\n");
                exit(1);
        }

        volname = argv[1];
        logfile = argv[2];

        fs = glfs_new (volname);
        if (!fs) {
                fprintf (stderr, "glfs_new: returned NULL\n");
                return -1;
        }

        ret = glfs_set_volfile_server (fs, "tcp", "localhost", 24007);
        LOG_ERR("glfs_set_volfile_server", ret);

        ret = glfs_set_logging (fs, logfile, 7);
        LOG_ERR("glfs_set_logging", ret);

        ret = glfs_init (fs);
        LOG_ERR("glfs_init", ret);

        fs2 = glfs_new (volname);
        if (!fs2) {
                fprintf (stderr, "glfs_new fs2: returned NULL\n");
                return 1;
        }

        ret = glfs_set_volfile_server (fs2, "tcp", "localhost", 24007);
        LOG_ERR("glfs_set_volfile_server-fs2", ret);

        ret = glfs_set_logging (fs2, logfile, 7);
        LOG_ERR("glfs_set_logging-fs2", ret);

        ret = glfs_init (fs2);
        LOG_ERR("glfs_init-fs2", ret);

        fd = glfs_creat(fs, filename, O_RDWR, 0644);
        if (fd <= 0) {
                ret = -1;
                LOG_ERR ("glfs_creat", ret);
        }
        fprintf (stderr, "glfs-create fd - %d\n", fd);

        fd2 = glfs_open(fs2, filename, O_RDWR | O_CREAT);
        if (fd2 <= 0) {
                ret = -1;
                LOG_ERR ("glfs_open-fs2", ret);
        }
        fprintf (stderr, "glfs-open fd2 - %d\n", fd2);

        do {
                if (cnt%2) {
                        fd_tmp = fd;
                        fs_tmp = fs;
                } else {
                        fd_tmp = fd2;
                        fs_tmp = fs2;
                }

                if (cnt < 3) {
                        writebuf = malloc(10);
                        if (writebuf) {
                                memcpy (writebuf, "abcd", 4);
                                ret = glfs_write (fd_tmp, writebuf, 4, 0);
                                if (ret <= 0)   {
                                        ret = -1;
                                        LOG_ERR ("glfs_write", ret);
                                } else {
                                        fprintf (stderr,
                                                 "glfs_write suceeded\n");
                                }
                                free(writebuf);
                        } else {
                                fprintf (stderr,
                                         "Could not allocate writebuf\n");
                                return -1;
                        }
                } else {
                        ret = glfs_lseek (fd_tmp, 0, SEEK_SET);
                        LOG_ERR ("glfs_lseek", ret);

                        ret = glfs_pread (fd_tmp, readbuf, 4, 0, 0);

                        if (ret <= 0) {
                                ret = -1;
                                LOG_ERR ("glfs_pread", ret);
                        } else {
                                fprintf (stderr, "glfs_read: %s\n", readbuf);
                        }
                }

                /* Open() fops seem to be not performed on server side until
                 * there are I/Os on that fd
                 */
                if (cnt >= 3) {
                        ret = glfs_h_poll_upcall(fs_tmp, &cbk);
                        LOG_ERR ("glfs_h_poll_upcall", ret);
                        if (cbk.glhandle) {
                                fprintf (stderr, " upcall event type - %d,"
                                                 " flags - %d\n" ,
                                         cbk.reason, cbk.flags);
                        } else {
                                fprintf (stderr,
                                         "Dint receive upcall notify event");
                                ret = -1;
                                goto out;
                        }
                }
                sleep(5);
        } while (cnt++ < 5);

        glfs_close(fd);
        LOG_ERR ("glfs_close", ret);

        glfs_close(fd2);
        LOG_ERR ("glfs_close-fd2", ret);

out:
        if (fs) {
                glfs_fini(fs);
                LOG_ERR("glfs_fini", ret);
        }

        if (fs2) {
                glfs_fini(fs2);
                LOG_ERR("glfs_fini-fs2", ret);
        }

        if (ret)
                exit(1);
        exit(0);
}


