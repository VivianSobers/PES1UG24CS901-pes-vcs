#include "pes.h"
#include "index.h"
#include "commit.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
 
// Forward declarations
int commit_walk(commit_walk_fn callback, void *ctx);

// ─── init ────────────────────────────────────────────────────────────────────

void cmd_init(void) {
    mkdir(PES_DIR, 0755);
    mkdir(OBJECTS_DIR, 0755);
    mkdir(".pes/refs", 0755);
    mkdir(REFS_DIR, 0755);

    FILE *f = fopen(HEAD_FILE, "w");
    if (f) { fprintf(f, "ref: refs/heads/main\n"); fclose(f); }

    printf("Initialized empty PES repository in .pes/\n");
}

// ─── add ─────────────────────────────────────────────────────────────────────

void cmd_add(int argc, char *argv[]) {
    if (argc < 3) { fprintf(stderr, "Usage: pes add <file>...\n"); return; }

    Index *index = calloc(1, sizeof(Index));
    if (!index) { fprintf(stderr, "error: out of memory\n"); return; }

    if (index_load(index) != 0) {
        fprintf(stderr, "error: failed to load index\n");
        free(index); return;
    }

    for (int i = 2; i < argc; i++) {
        if (index_add(index, argv[i]) != 0)
            fprintf(stderr, "error: failed to add '%s'\n", argv[i]);
        else
            printf("Added: %s\n", argv[i]);
    }
    free(index);
} 

// ─── status ──────────────────────────────────────────────────────────────────

void cmd_status(void) {
    Index *index = calloc(1, sizeof(Index));
    if (!index) { fprintf(stderr, "error: out of memory\n"); return; }
    index_load(index);
    index_status(index);
    free(index);
}

// ─── commit ──────────────────────────────────────────────────────────────────

void cmd_commit(int argc, char *argv[]) {
    const char *message = NULL;
    for (int i = 2; i < argc - 1; i++) {
        if (strcmp(argv[i], "-m") == 0) { message = argv[i + 1]; break; }
    }
    if (!message) {
        fprintf(stderr, "error: commit requires a message (-m \"message\")\n");
        return;
    }

    ObjectID commit_id;
    if (commit_create(message, &commit_id) != 0) {
        fprintf(stderr, "error: commit failed\n");
        return;
    }

    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(&commit_id, hex);
    printf("Committed: %.12s... %s\n", hex, message);
}

// ─── log ─────────────────────────────────────────────────────────────────────

static void log_callback(const ObjectID *id, const Commit *c, void *ctx) {
    (void)ctx;
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    time_t ts = (time_t)c->timestamp;
    char date_buf[64];
    struct tm *tm_info = localtime(&ts);
    strftime(date_buf, sizeof(date_buf), "%Y-%m-%d %H:%M:%S", tm_info);
    printf("commit %s\nAuthor: %s\nDate:   %s\n\n    %s\n\n",
           hex, c->author, date_buf, c->message);
}

void cmd_log(void) {
    if (commit_walk(log_callback, NULL) != 0)
        fprintf(stderr, "error: no commits yet (or failed to walk history)\n");
}

// ─── branch helpers (Phase 5 stubs) ─────────────────────────────────────────

static int branch_list(void) {
    FILE *f = fopen(HEAD_FILE, "r");
    if (!f) { fprintf(stderr, "error: not a PES repository\n"); return -1; }
    char line[512];
    char current_branch[256] = "main";
    if (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "ref: refs/heads/", 16) == 0) {
            line[strcspn(line, "\r\n")] = '\0';
            strncpy(current_branch, line + 16, sizeof(current_branch) - 1);
        }
    }
    fclose(f);
    printf("* %s\n", current_branch);
    return 0;
}

static int branch_create(const char *name) {
    char ref_path[512];
    snprintf(ref_path, sizeof(ref_path), "%s/%s", REFS_DIR, name);
    FILE *head = fopen(HEAD_FILE, "r");
    if (!head) return -1;
    char line[512];
    char commit_hash[HASH_HEX_SIZE + 1] = "";
    if (fgets(line, sizeof(line), head)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "ref: ", 5) == 0) {
            char src_path[512];
            snprintf(src_path, sizeof(src_path), "%s/%s", PES_DIR, line + 5);
            FILE *src = fopen(src_path, "r");
            if (src) {
                if (fgets(commit_hash, sizeof(commit_hash), src))
                    commit_hash[strcspn(commit_hash, "\r\n")] = '\0';
                fclose(src);
            }
        }
    }
    fclose(head);
    FILE *f = fopen(ref_path, "w");
    if (!f) return -1;
    fprintf(f, "%s\n", commit_hash);
    fclose(f);
    return 0;
}

static int branch_delete(const char *name) {
    char ref_path[512];
    snprintf(ref_path, sizeof(ref_path), "%s/%s", REFS_DIR, name);
    return unlink(ref_path);
}

static int checkout(const char *target) {
    char ref_path[512];
    snprintf(ref_path, sizeof(ref_path), "%s/%s", REFS_DIR, target);
    FILE *f = fopen(ref_path, "r");
    if (f) {
        fclose(f);
        FILE *head = fopen(HEAD_FILE, "w");
        if (!head) return -1;
        fprintf(head, "ref: refs/heads/%s\n", target);
        fclose(head);
        return 0;
    }
    ObjectID id;
    if (hex_to_hash(target, &id) == 0) {
        FILE *head = fopen(HEAD_FILE, "w");
        if (!head) return -1;
        fprintf(head, "%s\n", target);
        fclose(head);
        return 0;
    }
    return -1;
}

// ─── Phase 5 command wrappers (PROVIDED) ─────────────────────────────────────

void cmd_branch(int argc, char *argv[]) {
    if (argc == 2) {
        branch_list();
    } else if (argc == 3) {
        if (branch_create(argv[2]) == 0) printf("Created branch '%s'\n", argv[2]);
        else fprintf(stderr, "error: failed to create branch '%s'\n", argv[2]);
    } else if (argc == 4 && strcmp(argv[2], "-d") == 0) {
        if (branch_delete(argv[3]) == 0) printf("Deleted branch '%s'\n", argv[3]);
        else fprintf(stderr, "error: failed to delete branch '%s'\n", argv[3]);
    } else {
        fprintf(stderr, "Usage:\n pes branch\n pes branch <n>\n pes branch -d <n>\n");
    }
}

void cmd_checkout(int argc, char *argv[]) {
    if (argc < 3) { fprintf(stderr, "Usage: pes checkout <branch_or_commit>\n"); return; }
    if (checkout(argv[2]) == 0) printf("Switched to '%s'\n", argv[2]);
    else fprintf(stderr, "error: checkout failed. Do you have uncommitted changes?\n");
}

// ─── Command dispatch (PROVIDED) ─────────────────────────────────────────────

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: pes <command> [args]\n\nCommands:\n");
        fprintf(stderr, "  init               Create a new PES repository\n");
        fprintf(stderr, "  add <file>...      Stage files for commit\n");
        fprintf(stderr, "  status             Show working directory status\n");
        fprintf(stderr, "  commit -m <msg>    Create a commit from staged files\n");
        fprintf(stderr, "  log                Show commit history\n");
        fprintf(stderr, "  branch             List, create, or delete branches\n");
        fprintf(stderr, "  checkout <ref>     Switch branches or restore working tree\n");
        return 1;
    }
    const char *cmd = argv[1];
    if      (strcmp(cmd, "init")     == 0) cmd_init();
    else if (strcmp(cmd, "add")      == 0) cmd_add(argc, argv);
    else if (strcmp(cmd, "status")   == 0) cmd_status();
    else if (strcmp(cmd, "commit")   == 0) cmd_commit(argc, argv);
    else if (strcmp(cmd, "log")      == 0) cmd_log();
    else if (strcmp(cmd, "branch")   == 0) cmd_branch(argc, argv);
    else if (strcmp(cmd, "checkout") == 0) cmd_checkout(argc, argv);
    else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        return 1;
    }
    return 0;
}
