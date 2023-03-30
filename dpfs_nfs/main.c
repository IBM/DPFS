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
#include "dpfs_nfs.h"
#include "toml.h"

void usage()
{
    printf("virtionfs [-c config_path]\n");
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

    toml_table_t *nfs_conf = toml_table_in(conf, "nfs");
    if (!nfs_conf) {
        fprintf(stderr, "%s: missing [nfs] in config file", __func__);
        return -1;
    }
    
    toml_datum_t server = toml_string_in(nfs_conf, "server");
    if (!server.ok) {
        fprintf(stderr, "You must supply a server with `server` under [nfs]\n");
        return -1;
    }
    toml_datum_t export = toml_string_in(nfs_conf, "export");
    if (!export.ok) {
        fprintf(stderr, "You must supply a export with `export` under [nfs]\n");
        return -1;
    }

    printf("dpfs_nfs starting up!\n");
    printf("Connecting to %s:%s\n", server.u.s, export.u.s);

    dpfs_nfs_main(server.u.s, export.u.s, false, false, 1, conf_path);

    return 0;
}
