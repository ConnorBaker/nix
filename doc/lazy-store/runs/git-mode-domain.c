#include <git2.h>
#include <stdint.h>
#include <stdio.h>

/* Exact private layout from the pinned, patched src/libgit2/tree.h. */
struct git_tree_entry
{
    uint16_t attr;
    uint16_t filename_len;
    git_oid oid;
    const char * filename;
};

int main(void)
{
    struct git_tree_entry e = {.filename_len = 1, .filename = "a"};
    puts("raw_octal\tnormalized_octal");
    for (unsigned m = 0; m <= UINT16_MAX; m++) {
        e.attr = m;
        printf("%06o\t%06o\n", m, git_tree_entry_filemode(&e));
    }
    return 0;
}
