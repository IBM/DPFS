/*
#
# Copyright 2023- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#include <getopt.h>
#include <stdlib.h>
#include <errno.h>
#include "dpfs_fuse.h"
#include "toml.h"

#include "fuser.h"

void usage()
{
    printf("aio_mirror [-c config_path]\n");
}

int main(int argc, char **argv)
{
    char *conf_path = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "c:")) != -1) {
        switch (opt) {
            case 'c':
                conf_path = optarg;
                break;
            default: /* '?' */
                usage();
                exit(1);
        }
    }
    
    if (!conf_path) {
        fprintf(stderr, "A config file is required!");
        usage();
        return -1;
    }

    FILE *fp;
    char errbuf[200];
    
    // 1. Read and parse toml file
    fp = fopen(conf_path, "r");
    if (!fp) {
        fprintf(stderr, "%s: cannot open %s - %s", __func__,
                conf_path, strerror(errno));
        return -1;
    }
    
    toml_table_t *conf = toml_parse_file(fp, errbuf, sizeof(errbuf));
    fclose(fp);
    
    if (!conf) {
        fprintf(stderr, "%s: cannot parse - %s", __func__, errbuf);
        return -1;
    }

    toml_table_t *local_mirror_conf = toml_table_in(conf, "local_mirror");
    if (!local_mirror_conf) {
        fprintf(stderr, "%s: missing [local_mirror] in config file", __func__);
        return -1;
    }
    
    toml_datum_t dir = toml_string_in(local_mirror_conf, "dir");
    if (!dir.ok) {
        fprintf(stderr, "You must supply a directory to mirror with `dir` under [local_mirror]\n");
        return -1;
    }
    char *rp = realpath(dir.u.s, NULL);
    if (rp == NULL) {
        fprintf(stderr, "Could not parse dir %s, errno=%d\n", dir.u.s, errno);
        exit(errno);
    }
    toml_datum_t cached = toml_bool_in(local_mirror_conf, "cached");
    if (!cached.ok) {
        fprintf(stderr, "You must supply `cached` under [local_mirror]\n");
        return -1;
    }

    printf("virtiofuser starting up!\n");
    printf("Mirroring %s\n", rp);

    fuser_main(false, rp, cached.u.b, conf_path);
}
