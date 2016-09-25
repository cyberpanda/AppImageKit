
#include <glib.h>
#include <stdlib.h>

#include <stdio.h>
#include <argp.h>

#include <stdlib.h>
#include <fcntl.h>
#include "squashfuse.h"

#include <sys/types.h>
#include <sys/stat.h>

#include "binreloc.h"
#ifndef NULL
#define NULL ((void *) 0)
#endif

#include <libgen.h>

#include <unistd.h>
#include <string.h>

#include "elf.h"
#include "getsection.h"

extern int _binary_runtime_start;
extern int _binary_runtime_size;


static gint repeats = 2;
static gint max_size = 8;
static gboolean list = FALSE;
static gboolean verbose = FALSE;
static gboolean version = FALSE;
static gboolean sign = FALSE;
gchar **remaining_args = NULL;
gchar *updateinformation = NULL;
gchar *bintray_user = NULL;
gchar *bintray_repo = NULL;

// #####################################################################

static void die(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

/* Function that prints the contents of a squashfs file
* using libsquashfuse (#include "squashfuse.h") */
int sfs_ls(char* image) {
    sqfs_err err = SQFS_OK;
    sqfs_traverse trv;
    sqfs fs;
    
    unsigned long fs_offset = get_elf_size(image);
    
    if ((err = sqfs_open_image(&fs, image, fs_offset)))
        die("sqfs_open_image error, TODO: Implement offset");
    
    if ((err = sqfs_traverse_open(&trv, &fs, sqfs_inode_root(&fs))))
        die("sqfs_traverse_open error");
    while (sqfs_traverse_next(&trv, &err)) {
        if (!trv.dir_end) {
            printf("%s\n", trv.path);
        }
    }
    if (err)
        die("sqfs_traverse_next error");
    sqfs_traverse_close(&trv);
    
    sqfs_fd_close(fs.fd);
    return 0;
}

/* Generate a squashfs filesystem using mksquashfs on the $PATH 
* execlp(), execvp(), and execvpe() search on the $PATH */
int sfs_mksquashfs(char *source, char *destination) {
    pid_t parent = getpid();
    pid_t pid = fork();
    
    if (pid == -1) {
        // error, failed to fork()
        return(-1);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
    } else {
        // we are the child
        execlp("mksquashfs", "mksquashfs", source, destination, "-root-owned", "-noappend", (char *)0);
        perror("execlp");   // execlp() returns only on error
        return(-1); // exec never returns
    }
    return(0);
}

/* Generate a squashfs filesystem
* The following would work if we link to mksquashfs.o after we renamed 
* main() to mksquashfs_main() in mksquashfs.c but we don't want to actually do
* this because squashfs-tools is not under a permissive license
* i *nt sfs_mksquashfs(char *source, char *destination) {
* char *child_argv[5];
* child_argv[0] = NULL;
* child_argv[1] = source;
* child_argv[2] = destination;
* child_argv[3] = "-root-owned";
* child_argv[4] = "-noappend";
* mksquashfs_main(5, child_argv);
* }
*/

gchar* find_first_matching_file(const gchar *real_path, const gchar *pattern) {
    GDir *dir;
    gchar *full_name;
    gchar *resulting;
    dir = g_dir_open(real_path, 0, NULL);
    if (dir != NULL) {
        const gchar *entry;
        while ((entry = g_dir_read_name(dir)) != NULL) {
            full_name = g_build_filename(real_path, entry, NULL);
            if (! g_file_test(full_name, G_FILE_TEST_IS_DIR)) {
                if(g_pattern_match_simple(pattern, entry))
                    return(full_name);
            }
            else {
                resulting = find_first_matching_file(full_name, pattern);
                if(resulting)
                    return(resulting);
            }
        }
        g_dir_close(dir);
        return NULL;
    }
    else {
        g_warning("%s: %s", real_path, g_strerror(errno));
    }
}

gchar* find_first_matching_file_nonrecursive(const gchar *real_path, const gchar *pattern) {
    GDir *dir;
    gchar *full_name;
    gchar *resulting;
    dir = g_dir_open(real_path, 0, NULL);
    if (dir != NULL) {
        const gchar *entry;
        while ((entry = g_dir_read_name(dir)) != NULL) {
            full_name = g_build_filename(real_path, entry, NULL);
            if (g_file_test(full_name, G_FILE_TEST_IS_REGULAR)) {
                if(g_pattern_match_simple(pattern, entry))
                    return(full_name);
            }
        }
        g_dir_close(dir);
        return NULL;
    }
    else { 
        g_warning("%s: %s", real_path, g_strerror(errno));
    }
}

gchar* get_desktop_entry(GKeyFile *kf, char *key) {
    gchar *value = g_key_file_get_string (kf, "Desktop Entry", key, NULL);
    if (! value){
        fprintf(stderr, "%s entry not found in desktop file\n", key);
        exit(1);
    }
    return value;
}

/* in-place modification of the string, and assuming the buffer pointed to by
* line is large enough to hold the resulting string*/
static void replacestr(char *line, const char *search, const char *replace)
{
    char *sp;
    
    if ((sp = strstr(line, search)) == NULL) {
        return;
    }
    int search_len = strlen(search);
    int replace_len = strlen(replace);
    int tail_len = strlen(sp+search_len);
    
    memmove(sp+replace_len,sp+search_len,tail_len+1);
    memcpy(sp, replace, replace_len);
    
    /* Do it recursively again until no more work to do */
    
    if ((sp = strstr(line, search))) {
        replacestr(line, search, replace);
    }
}

// #####################################################################

static GOptionEntry entries[] =
{
    // { "repeats", 'r', 0, G_OPTION_ARG_INT, &repeats, "Average over N repetitions", "N" },
    { "list", 'l', 0, G_OPTION_ARG_NONE, &list, "List files in SOURCE AppImage", NULL },
    { "updateinformation", 'u', 0, G_OPTION_ARG_STRING, &updateinformation, "Embed update information STRING; if zsyncmake is installed, generate zsync file", NULL },
    { "bintray-user", NULL, 0, G_OPTION_ARG_STRING, &bintray_user, "Bintray user name", NULL },
    { "bintray-repo", NULL, 0, G_OPTION_ARG_STRING, &bintray_repo, "Bintray repository", NULL },
    { "version", NULL, 0, G_OPTION_ARG_NONE, &version, "Show version number", NULL },
    { "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose, "Produce verbose output", NULL },
    { "sign", 's', 0, G_OPTION_ARG_NONE, &sign, "Sign with gpg2", NULL },
    { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &remaining_args, NULL },
    { NULL }
};

int
main (int argc, char *argv[])
{
    GError *error = NULL;
    GOptionContext *context;
    char command[PATH_MAX];
    
    context = g_option_context_new ("SOURCE [DESTINATION] - Generate, extract, and inspect AppImages");
    g_option_context_add_main_entries (context, entries, NULL);
    // g_option_context_add_group (context, gtk_get_option_group (TRUE));
    if (!g_option_context_parse (context, &argc, &argv, &error))
    {
        g_print("option parsing failed: %s\n", error->message);
        exit(1);
    }

    if(version){
        fprintf(stderr,"Version: %s\n", VERSION_NUMBER);
        exit(0);
    }
        
    /* Check for dependencies here. Better fail early if they are not present. */
    if(! g_find_program_in_path ("mksquashfs"))
        die("mksquashfs is missing but required, please install it");
    if(! g_find_program_in_path ("zsyncmake"))
        g_print("WARNING: zsyncmake is missing, please install it if you want to use binary delta updates\n");
    if(! g_find_program_in_path ("appstreamcli"))
        g_print("WARNING: appstreamcli is missing, please install it if you want to use AppStream metadata\n");
    if(! g_find_program_in_path ("appstream-util"))
        g_print("WARNING: appstream-util is missing, please install it if you want to use AppStream metadata\n");
    if(! g_find_program_in_path ("gpg2"))
        g_print("WARNING: gpg2 is missing, please install it if you want to create digital signatures\n");
    if(! g_find_program_in_path ("sha256sum"))
        g_print("WARNING: sha256sum is missing, please install it if you want to create digital signatures\n");
    
    if(!&remaining_args[0])
        die("SOURCE is missing");
    
    /* If in list mode */
    if (list){
        sfs_ls(remaining_args[0]);
        exit(0);
    }
    
    /* If the first argument is a directory, then we assume that we should package it */
    if (g_file_test (remaining_args[0], G_FILE_TEST_IS_DIR)){
        char *destination;
        char source[PATH_MAX];
        realpath(remaining_args[0], source);
        
        /* Check if *.desktop file is present in source AppDir */
        gchar *desktop_file = find_first_matching_file_nonrecursive(source, "*.desktop");
        if(desktop_file == NULL){
            die("$ID.desktop file not found");
        }
        if(verbose)
            fprintf (stdout, "Desktop file: %s\n", desktop_file);
        
        /* Read information from .desktop file */
        GKeyFile *kf = g_key_file_new ();
        if (!g_key_file_load_from_file (kf, desktop_file, 0, NULL))
            die(".desktop file cannot be parsed");
        
        if(verbose){
            fprintf (stderr,"Name: %s\n", get_desktop_entry(kf, "Name"));
            fprintf (stderr,"Icon: %s\n", get_desktop_entry(kf, "Icon"));
            fprintf (stderr,"Exec: %s\n", get_desktop_entry(kf, "Exec"));
            fprintf (stderr,"Comment: %s\n", get_desktop_entry(kf, "Comment"));
            fprintf (stderr,"Type: %s\n", get_desktop_entry(kf, "Type"));
            fprintf (stderr,"Categories: %s\n", get_desktop_entry(kf, "Categories"));
        }
        
        /* Determine the architecture */
        gchar* archfile;
        /* We use the next best .so that we can find to determine the architecture */
        archfile = find_first_matching_file(source, "*.so.*");
        if(!archfile)
        {
            /* If we found no .so we try to guess the main executable - this might be a script though */
            char guessed_bin_path[PATH_MAX];
            sprintf (guessed_bin_path, "%s/usr/bin/%s", source,  g_strsplit_set(get_desktop_entry(kf, "Exec"), " ", -1)[0]);
            archfile = guessed_bin_path;
        }
        if(verbose)
            fprintf (stderr,"File used for determining architecture: %s\n", archfile);
        FILE *fp;
        char line[PATH_MAX];
        char command[PATH_MAX];
        sprintf (command, "/usr/bin/file -L -N -b %s", archfile);
        fp = popen(command, "r");
        if (fp == NULL)
            die("Failed to run file command");
        fgets(line, sizeof(line)-1, fp);
        gchar* arch = g_strstrip(g_strsplit_set(line, ",", -1)[1]);
        replacestr(arch, "-", "_");
        fprintf (stderr,"Arch: %s\n", arch+1);
        pclose(fp);
        
        if(!arch)
        {
            printf("The architecture could not be determined, assuming 'all'\n");
            arch="all";
        }
        
        char app_name_for_filename[PATH_MAX];
        sprintf(app_name_for_filename, "%s", get_desktop_entry(kf, "Name"));
        replacestr(app_name_for_filename, " ", "_");
        
        if(verbose)
            fprintf (stderr,"App name for filename: %s\n", app_name_for_filename);
        
        if (remaining_args[1]) {
            destination = remaining_args[1];
        } else {
            /* No destination has been specified, to let's construct one
            * TODO: Find out the architecture and use a $VERSION that might be around in the env */
            char dest_path[PATH_MAX];
            sprintf (dest_path, "%s-%s.AppImage", app_name_for_filename, arch);
            
            if(verbose)
                fprintf (stderr,"dest_path: %s\n", dest_path);
            
            if(g_environ_getenv (g_get_environ (), "VERSION"))
                sprintf (dest_path, "%s-%s-%s.AppImage", app_name_for_filename,
                        g_environ_getenv (g_get_environ (), "VERSION"), arch);
                
                destination = dest_path;
            replacestr(destination, " ", "_");
            
            // destination = basename(br_strcat(source, ".AppImage"));
            fprintf (stdout, "DESTINATION not specified, so assuming %s\n", destination);
        }
        fprintf (stdout, "%s should be packaged as %s\n", source, destination);
        
        /* Check if the Icon file is how it is expected */
        gchar* icon_name = get_desktop_entry(kf, "Icon");
        char icon_file_path[PATH_MAX];
        sprintf(icon_file_path, "%s/%s.png", source, icon_name);
        if (! g_file_test(icon_file_path, G_FILE_TEST_IS_REGULAR)){
            fprintf (stderr, "%s not present but defined in desktop file\n", icon_file_path);
            exit(1);
        }
        
        /* Check if .DirIcon is present in source AppDir */
        gchar *diricon_path = g_build_filename(source, ".DirIcon", NULL);
        if (! g_file_test(diricon_path, G_FILE_TEST_IS_REGULAR)){
            fprintf (stderr, "Creating .DirIcon symlink based on information from desktop file\n");
            int res = symlink(basename(icon_file_path), diricon_path);
            if(res)
                die("Could not symlink .DirIcon");
        }
        
        /* Check if AppStream upstream metadata is present in source AppDir */
        char application_id[PATH_MAX];
        sprintf (application_id,  "%s", basename(desktop_file));
        replacestr(application_id, ".desktop", ".appdata.xml");
        gchar *appdata_path = g_build_filename(source, "/usr/share/metainfo/", application_id, NULL);
        if (! g_file_test(appdata_path, G_FILE_TEST_IS_REGULAR)){
            fprintf (stderr, "WARNING: AppStream upstream metadata is missing, please consider creating it\n");
            fprintf (stderr, "         in usr/share/metainfo/%s\n", application_id);
            fprintf (stderr, "         Please see https://www.freedesktop.org/software/appstream/docs/chap-Quickstart.html#sect-Quickstart-DesktopApps\n");
            fprintf (stderr, "         for more information.\n");
            /* As a courtesy, generate one to be filled by the user */
            if(g_find_program_in_path ("appstream-util")) {
                sprintf (command, "%s appdata-from-desktop %s %s", g_find_program_in_path ("appstream-util"), desktop_file, appdata_path);
                int ret = system(command);
                if (ret != 0)
                    die("Failed to generate a template");
                fprintf (stderr, "A template has been generated in in %s, please edit it\n", appdata_path);
                exit(1);
            }
        } else {
            fprintf (stderr, "AppStream upstream metadata found in usr/share/metainfo/%s\n", application_id);
            /* Use ximion's appstreamcli to make sure that desktop file and appdata match together */
            if(g_find_program_in_path ("appstreamcli")) {
                sprintf (command, "%s validate-tree %s", g_find_program_in_path ("appstreamcli"), source);
                int ret = system(command);
                if (ret != 0)
                    die("Failed to validate AppStream information with appstreamcli");
            }
            /* It seems that hughsie's appstream-util does additional validations */
            if(g_find_program_in_path ("appstream-util")) {
                sprintf (command, "%s validate-relax %s", g_find_program_in_path ("appstream-util"), appdata_path);
                int ret = system(command);
                if (ret != 0)
                    die("Failed to validate AppStream information with appstream-util");
            }
        }
        
        /* mksquashfs can currently not start writing at an offset,
        * so we need a tempfile. https://github.com/plougher/squashfs-tools/pull/13
        * should hopefully change that. */
        char *tempfile;
        fprintf (stderr, "Generating squashfs...\n");
        tempfile = br_strcat(destination, ".temp");
        int result = sfs_mksquashfs(source, tempfile);
        if(result != 0)
            die("sfs_mksquashfs error");
        
        fprintf (stderr, "Generating AppImage...\n");
        FILE *fpsrc = fopen(tempfile, "rb");
        if (fpsrc == NULL) {
            die("Not able to open the tempfile for reading, aborting");
        }
        FILE *fpdst = fopen(destination, "w");
        if (fpdst == NULL) {
            die("Not able to open the destination file for writing, aborting");
        }
        
        /* runtime is embedded into this executable
        * http://stupefydeveloper.blogspot.de/2008/08/cc-embed-binary-data-into-elf.html */
        int size = (int)&_binary_runtime_size;
        char *data = (char *)&_binary_runtime_start;
        if (verbose)
            printf("Size of the embedded runtime: %d bytes\n", size);
        fwrite(data, size, 1, fpdst);
        fseek (fpdst, 0, SEEK_END);
        char byte;
        
        while (!feof(fpsrc))
        {
            fread(&byte, sizeof(char), 1, fpsrc);
            fwrite(&byte, sizeof(char), 1, fpdst);
        }
        
        fclose(fpsrc);
        fclose(fpdst);
        
        fprintf (stderr, "Marking the AppImage as executable...\n");
        if (chmod (destination, 0755) < 0) {
            printf("Could not set executable bit, aborting\n");
            exit(1);
        }
        if(unlink(tempfile) != 0) {
            die("Could not delete the tempfile, aborting");
        }
        
        if(bintray_user != NULL){
            if(bintray_repo != NULL){
                char buf[1024];
                sprintf(buf, "bintray-zsync|%s|%s|%s|%s-_latestVersion-%s.AppImage.zsync", bintray_user, bintray_repo, app_name_for_filename, app_name_for_filename, arch);
                updateinformation = buf;
                printf("%s\n", updateinformation);
            }
        }
        
        /* If updateinformation was provided, then we check and embed it */
        if(updateinformation != NULL){
            if(!g_str_has_prefix(updateinformation,"zsync|"))
                if(!g_str_has_prefix(updateinformation,"bintray-zsync|"))
                    die("The provided updateinformation is not in a recognized format");
                
            gchar **ui_type = g_strsplit_set(updateinformation, "|", -1);
                        
            if(verbose)
                printf("updateinformation type: %s\n", ui_type[0]);
            /* TODO: Further checking of the updateinformation */

            /* As a courtesy, we also generate the zsync file */
            gchar *zsyncmake_path = g_find_program_in_path ("zsyncmake");
            if(!zsyncmake_path){
                fprintf (stderr, "zsyncmake is not installed, skipping\n");
            } else {
                fprintf (stderr, "zsyncmake is installed and updateinformation is provided,"
                "hence generating zsync file\n");
                sprintf (command, "%s %s -u %s", zsyncmake_path, destination, basename(destination));
                fp = popen(command, "r");
                if (fp == NULL)
                    die("Failed to run zsyncmake command");            
            }
            
            unsigned long ui_offset = 0;
            unsigned long ui_length = 0;
            get_elf_section_offset_and_lenghth(destination, ".upd_info", &ui_offset, &ui_length);
            if(verbose)
                printf("ui_offset: %lu\n", ui_offset);
            if(verbose)
                printf("ui_length: %lu\n", ui_length);
            if(ui_offset == 0) {
                die("Could not determine offset for updateinformation");
            } else {
                if(strlen(updateinformation)>ui_length)
                    die("updateinformation does not fit into segment, aborting");
                FILE *fpdst2 = fopen(destination, "r+");
                if (fpdst2 == NULL)
                    die("Not able to open the destination file for writing, aborting");
                fseek(fpdst2, ui_offset, SEEK_SET);
                // fseek(fpdst2, ui_offset, SEEK_SET);
                // fwrite(0x00, 1, 1024, fpdst); // FIXME: Segfaults; why?
                // fseek(fpdst, ui_offset, SEEK_SET);
                fwrite(updateinformation, strlen(updateinformation), 1, fpdst2);
                fclose(fpdst2);
            }
        }

        if(sign != NULL){
            /* The user has indicated that he wants to sign */
            gchar *gpg2_path = g_find_program_in_path ("gpg2");
            gchar *sha256sum_path = g_find_program_in_path ("sha256sum");
            if(!gpg2_path){
                fprintf (stderr, "gpg2 is not installed, cannot sign\n");
            }
            else if(!sha256sum_path){
                fprintf (stderr, "sha256sum is not installed, cannot sign\n");
            } else {
                fprintf (stderr, "gpg2 and sha256sum are installed and user requested to sign, "
                "hence signing\n");
                char *digestfile;
                digestfile = br_strcat(destination, ".digest");
                char *ascfile;
                ascfile = br_strcat(destination, ".digest.asc");
                if (g_file_test (digestfile, G_FILE_TEST_IS_REGULAR))
                    unlink(digestfile);
                sprintf (command, "%s %s", sha256sum_path, destination);
                if(verbose)
                    fprintf (stderr, "%s\n", command);
                fp = popen(command, "r");
                if (fp == NULL)
                    die("sha256sum command did not succeed");
                char output[1024];
                fgets(output, sizeof(output)-1, fp);
                if(verbose)
                    printf("sha256sum: %s\n", g_strsplit_set(output, " ", -1)[0]);
                FILE *fpx = fopen(digestfile, "w");
                if (fpx != NULL)
                {
                    fputs(g_strsplit_set(output, " ", -1)[0], fpx);
                    fclose(fpx);
                }
                if(WEXITSTATUS(pclose(fp)) != 0)
                    die("sha256sum command did not succeed");
                if (g_file_test (ascfile, G_FILE_TEST_IS_REGULAR))
                    unlink(ascfile);
                sprintf (command, "%s --detach-sign --armor %s", gpg2_path, digestfile);
                if(verbose)
                    fprintf (stderr, "%s\n", command);
                fp = popen(command, "r");
                if(WEXITSTATUS(pclose(fp)) != 0)
                    die("gpg2 command did not succeed");
                unsigned long sig_offset = 0;
                unsigned long sig_length = 0;
                get_elf_section_offset_and_lenghth(destination, ".sha256_sig", &sig_offset, &sig_length);
                if(verbose)
                    printf("sig_offset: %lu\n", sig_offset);
                if(verbose)
                    printf("sig_length: %lu\n", sig_length);
                if(sig_offset == 0) {
                    die("Could not determine offset for signature");
                } else {
                    FILE *fpdst3 = fopen(destination, "r+");
                    if (fpdst3 == NULL)
                        die("Not able to open the destination file for writing, aborting");
//                    if(strlen(updateinformation)>sig_length)
//                        die("signature does not fit into segment, aborting");
                    fseek(fpdst3, sig_offset, SEEK_SET);
                    FILE *fpsrc2 = fopen(ascfile, "rb");
                    if (fpsrc2 == NULL) {
                        die("Not able to open the asc file for reading, aborting");
                    }
                    char byte;
                    while (!feof(fpsrc2))
                    {
                        fread(&byte, sizeof(char), 1, fpsrc2);
                        fwrite(&byte, sizeof(char), 1, fpdst3);
                    }
                    fclose(fpsrc2);
                    fclose(fpdst3);
                }
                if (g_file_test (ascfile, G_FILE_TEST_IS_REGULAR))
                    unlink(ascfile);
                if (g_file_test (digestfile, G_FILE_TEST_IS_REGULAR))
                    unlink(digestfile);
            }
        }     
        fprintf (stderr, "Success\n");
        }
    
    /* If the first argument is a regular file, then we assume that we should unpack it */
    if (g_file_test (remaining_args[0], G_FILE_TEST_IS_REGULAR)){
        fprintf (stdout, "%s is a file, assuming it is an AppImage and should be unpacked\n", remaining_args[0]);
        die("To be implemented");
    }
    
    return 0;    
}