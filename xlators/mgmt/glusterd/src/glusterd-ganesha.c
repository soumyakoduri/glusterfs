#ifndef _CONFIG_H
#define _CONFIG_H
#include "config.h"
#endif

#include "common-utils.h"
#include "glusterd.h"
#include "glusterd-op-sm.h"
#include "glusterd-store.h"
#include "glusterd-utils.h"
#define MAXBUF 1024
#define DELIM "=\""

int is_ganesha_host (void)
{
        char *host_from_file             = NULL;
        glusterd_conf_t *priv            = NULL;
        glusterd_volinfo_t *volinfo      = NULL;
        glusterd_brickinfo_t *brickinfo  = NULL;
        char *hostname                   = NULL;
        FILE                             *fp;
        char                             line[MAXBUF];
        int     ret                      = 0;
        int    i                         = 1;

        priv =  THIS->private;
        GF_ASSERT(priv);

        fp = fopen (GANESHA_HA_CONF, "r");

        if (fp == NULL) {
                gf_log ("", GF_LOG_INFO, "couldn't open the file");
                return 0;
        }

        list_for_each_entry (volinfo, &priv->volumes, vol_list) {
                        list_for_each_entry (brickinfo, &volinfo->bricks, brick_list) {
                        hostname = brickinfo->hostname;
        }
        break;
        }
        while (fgets (line, sizeof(line), fp) != NULL) {
                if (i == 4) {
                        host_from_file = strstr ((char *)line, DELIM);
                        host_from_file = host_from_file + strlen(DELIM);
                        break;
                }
                i++;
        }
        i = 0;
        i = strlen(host_from_file);
        host_from_file[i - 2] = '\0';

        if (strcmp (hostname, host_from_file) == 0) {
                gf_log ("", GF_LOG_DEBUG, "One of NFS-Ganesha hosts found");
                ret = 1;
        }
        return ret;
}


int create_export_config (char *volname, char **op_errstr)
{
        runner_t                runner                     = {0,};
        int                     ret                        = -1;
        runinit (&runner);

        runner_add_args (&runner, "sh", GANESHA_PREFIX"/create-export-ganesha.sh", volname, NULL);
        ret = runner_run(&runner);

        if (ret == -1)
        gf_asprintf (op_errstr, "Failed to create NFS-Ganesha export config file.");

        return ret;
}

int  ganesha_manage_export (dict_t *dict, char *value, char **op_errstr)
{
        runner_t                runner = {0,};
        int                     ret    =  -1;
        FILE *fp;
        char                    str[1024];
        char                    *hostname;
        glusterd_volinfo_t     *volinfo = NULL;
        char                   *volname = NULL;
        glusterd_brickinfo_t *brickinfo = NULL;
        int i = 1;
        runinit (&runner);

        ret = dict_get_str (dict, "volname", &volname);
        if (ret) {
                gf_log ("", GF_LOG_ERROR, "Unable to get volume name");
                goto out;
        }

        ret = glusterd_volinfo_find (volname, &volinfo);
        if (ret) {
                gf_log ("", GF_LOG_ERROR, FMTSTR_CHECK_VOL_EXISTS,
                        volname);
                goto out;
        }

        fp = fopen (GANESHA_HA_CONF, "r");

        if (fp == NULL) {
                gf_log ("", GF_LOG_INFO, "couldn't open the file");
                return -1;
        }

        /* Read the hostname of the current node */
        list_for_each_entry (brickinfo, &volinfo->bricks, brick_list)
        {
                if (!uuid_compare (brickinfo->uuid, MY_UUID)) {
                /* Reading GANESHA_HA_CONF to get the host names listed */
                while (fgets (str, sizeof(str), fp) != NULL) {
                        if (i == 5) {
                        hostname = strtok (str, " ,.-\"");
                        while (hostname != NULL) {
                        hostname = strtok (NULL, " ,. -\"");
                if (strcmp (hostname, brickinfo->hostname) == 0) {
                                if (strcmp (value, "on") == 0)
                                create_export_config (volname, op_errstr);
                        runner_add_args (&runner, "sh", GANESHA_PREFIX"/dbus-send.sh",
                                        value, volname, NULL);
                        ret = runner_run (&runner);
                        }
                }
                fclose(fp);
                break;
                }
                i++;
                }
        }
        }

        if (ret == -1)
        gf_asprintf(op_errstr, "Dynamic export addition/deletion failed."
                        "Please see log file for details");
out:
        return ret;
}

int tear_down_cluster(void)
{
        int     ret     =       0;
        runner_t runner =       {0,};
        if (is_ganesha_host()) {
        runinit (&runner);
        runner_add_args (&runner, "sh", GANESHA_PREFIX"/ganesha-ha.sh", "teardown", NULL);
        ret = runner_run(&runner);
        }
        return ret;
}


int setup_cluster(void)
{
        int ret         = 0;
        runner_t runner = {0,};
        if (is_ganesha_host()) {
        runinit (&runner);
        runner_add_args (&runner, "sh", GANESHA_PREFIX"/ganesha-ha.sh", "setup", NULL);
        ret =  runner_run (&runner);
        }
        return ret;
}


int stop_ganesha (char **op_errstr)
{
        runner_t                runner                     = {0,};
        int                     ret                        = 1;

        ret = tear_down_cluster();
        if (ret == -1)
        gf_asprintf (op_errstr, "Cleanup of NFS-Ganesha HA config failed.");

        return ret;
}

int start_ganesha (dict_t *dict, char **op_errstr)
{

        runner_t                runner                     = {0,};
        int                     ret                        = -1;
        char                    key[1024]                  = {0,};
        char                    *hostname                  = NULL;
        dict_t *vol_opts                                   = NULL;
        glusterd_volinfo_t *volinfo                        = NULL;
        int count                                          = 0;
        dict_t *dict1                                      = NULL;
        char *volname                                      = NULL;
        glusterd_conf_t *priv                              = NULL;

        priv =  THIS->private;
        GF_ASSERT(priv);

        dict1 = dict_new();
        if (!dict1)
                goto out;

        list_for_each_entry (volinfo, &priv->volumes, vol_list) {
                ret = dict_set_str (dict1, key, volinfo->volname);
                if (ret)
                        goto out;
                vol_opts = volinfo->dict;
                ret = dict_set_str (vol_opts, "nfs.disable", "on");
        }

        glusterd_nfs_server_stop();

        runinit(&runner);
        runner_add_args (&runner, "/usr/bin/ganesha.nfsd",
                         "-L", "/nfs-ganesha-op.log",
                         "-f", CONFDIR"/nfs-ganesha.conf",
                         "-N", "NIV_FULL_DEBUG", "-d", NULL);

        ret =  runner_run (&runner);
        if (ret == -1) {
        gf_asprintf (op_errstr, "NFS-Ganesha failed to start."
                        "Please see log file for details");
        goto out;
        }

        ret = setup_cluster();
        if (ret == -1) {
        gf_asprintf (op_errstr, "Failed to set up HA config for NFS-Ganesha."
                        "Please check the log file for details");
        goto out;
        }

out:
        return ret;
}

int glusterd_handle_ganesha_op(dict_t *dict, char **op_errstr, char *key, char *value)
{

        int32_t                 ret          = -1;
        char                   *volname      = NULL;
        xlator_t               *this         = NULL;
        static int             export_id     = 1;
        glusterd_volinfo_t     *volinfo      = NULL;
        char *option =  NULL;

        GF_ASSERT(dict);
        GF_ASSERT(op_errstr);

        if (strcmp (key, "ganesha.enable") == 0) {
                ret =  ganesha_manage_export(dict, value, op_errstr);
                if (ret < 0)
                        goto out;
        }

        if (strcmp (key, "features.ganesha") == 0) {
                if (strcmp (value, "enable") == 0) {
                        ret = start_ganesha(dict, op_errstr);
                        if (ret < 0)
                                goto out;
                }

        else if (strcmp (value, "disable") == 0) {
                        ret = stop_ganesha (op_errstr);
                        if (ret < 0)
                                goto out;
                }
}

out:
        return ret;
}

