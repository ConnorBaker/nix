#include <git2.h>
#include <stdio.h>

static int cb(const char * path, unsigned flags, void * payload)
{
    printf("%u\t%s\n", flags, path);
    return 0;
}

int main(int argc, char ** argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s REPOSITORY\n", argv[0]);
        return 2;
    }
    git_repository * r = 0;
    git_libgit2_init();
    if (git_repository_open(&r, argv[1]))
        return 1;
    git_status_options o = GIT_STATUS_OPTIONS_INIT;
    o.flags = GIT_STATUS_OPT_INCLUDE_UNMODIFIED | GIT_STATUS_OPT_EXCLUDE_SUBMODULES;
    int e = git_status_foreach_ext(r, &o, cb, 0);
    git_repository_free(r);
    git_libgit2_shutdown();
    return e ? 1 : 0;
}
