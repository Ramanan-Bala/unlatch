// SPDX-License-Identifier: GPL-3.0-or-later
#include "check_report.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>

typedef struct {
    FILE *out;
    bool json;
    uint64_t n;
} report_ctx_t;

static void json_string(FILE *out, const char *s)
{
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\')
            fprintf(out, "\\%c", *p);
        else if (*p < 0x20)
            fprintf(out, "\\u%04x", *p);
        else
            fputc(*p, out);
    }
    fputc('"', out);
}

static void on_problem(void *ctx, const ue_check_problem_t *p)
{
    report_ctx_t *r = ctx;

    if (!r->json) {
        fprintf(r->out, "%-18s %s%s%s%s\n", ue_check_kind_name(p->kind), p->path, p->path[0] ? ": " : "", p->detail,
                p->repairable ? " (repairable)" : "");
        r->n++;
        return;
    }
    fprintf(r->out, "%s\n    {\"kind\": ", r->n++ ? "," : "");
    json_string(r->out, ue_check_kind_name(p->kind));
    fprintf(r->out, ", \"repairable\": %s, \"record\": %" PRIu64, p->repairable ? "true" : "false", p->rec);
    if (p->other != 0 && p->other != UINT64_MAX)
        fprintf(r->out, ", \"other\": %" PRIu64, p->other);
    if (p->count)
        fprintf(r->out, ", \"lcn\": %" PRIu64 ", \"count\": %" PRIu64, p->lcn, p->count);
    fputs(", \"path\": ", r->out);
    json_string(r->out, p->path);
    fputs(", \"detail\": ", r->out);
    json_string(r->out, p->detail);
    fputc('}', r->out);
}

int check_report(const ue_io_t *io, bool repair, bool json, FILE *out)
{
    report_ctx_t r = { .out = out, .json = json };
    ue_check_result_t res;
    int err, code;

    if (json)
        fprintf(out, "{\n  \"schema\": 1,\n  \"problems\": [");
    err = ue_check(io, repair ? UE_CHECK_REPAIR : 0, on_problem, &r, &res);
    if (err)
        code = CHECK_FAILED;
    else if (res.remaining)
        code = CHECK_PROBLEMS;
    else
        code = res.problems ? CHECK_REPAIRED : CHECK_CLEAN;

    const char *why = err == EPERM ? "Windows did not shut down fully: it must finish its own changes first"
                    : err == EROFS ? "the volume cannot be opened for writing"
                    : err == EINVAL ? "not an NTFS volume"
                    : err ? strerror(err) : NULL;
    if (json) {
        fprintf(out, "%s],\n", r.n ? "\n  " : "");
        fprintf(out, "  \"state\": \"%s\",\n", ue_vol_state_name(res.state));
        fprintf(out, "  \"records\": %" PRIu64 ", \"recordsInUse\": %" PRIu64 ", \"directories\": %" PRIu64
                ", \"entries\": %" PRIu64 ",\n", res.records, res.records_in_use, res.directories, res.entries);
        fprintf(out, "  \"clusters\": %" PRIu64 ", \"clustersInUse\": %" PRIu64 ",\n", res.clusters, res.clusters_in_use);
        fprintf(out, "  \"lostRecords\": %" PRIu64 ", \"lostClusters\": %" PRIu64 ",\n", res.lost_records,
                res.lost_clusters);
        fprintf(out, "  \"found\": %" PRIu64 ", \"repairable\": %" PRIu64 ", \"repaired\": %" PRIu64
                ", \"remaining\": %" PRIu64 ",\n", res.problems, res.repairable, res.repaired, res.remaining);
        fprintf(out, "  \"entriesRemoved\": %" PRIu64 ", \"orphansRelinked\": %" PRIu64 ", \"orphansFound\": %" PRIu64
                ", \"mirrorRepaired\": %s,\n", res.entries_removed, res.orphans_relinked, res.orphans_found,
                res.mirror_repaired ? "true" : "false");
        fprintf(out, "  \"repairRan\": %s, \"dirtyCleared\": %s,\n", res.repair_ran ? "true" : "false",
                res.dirty_cleared ? "true" : "false");
        fprintf(out, "  \"exitCode\": %d", code);
        if (why) {
            fputs(",\n  \"error\": ", out);
            json_string(out, why);
        }
        fputs("\n}\n", out);
        return code;
    }
    if (err) {
        fprintf(out, "check failed: %s\n", why);
        return code;
    }
    fprintf(out, "state:        %s\n", ue_vol_state_name(res.state));
    fprintf(out, "records:      %" PRIu64 " in use of %" PRIu64 "; %" PRIu64 " directories, %" PRIu64 " entries\n",
            res.records_in_use, res.records, res.directories, res.entries);
    fprintf(out, "clusters:     %" PRIu64 " in use of %" PRIu64 "\n", res.clusters_in_use, res.clusters);
    fprintf(out, "lost:         %" PRIu64 " records, %" PRIu64 " clusters (marked, unused: space only)\n",
            res.lost_records, res.lost_clusters);
    fprintf(out, "found:        %" PRIu64 " (%" PRIu64 " repairable)\n", res.problems, res.repairable);
    if (repair) {
        fprintf(out, "repaired:     %" PRIu64 " bitmap bits, %" PRIu64 " folder entries removed, %" PRIu64
                " orphans put back (%" PRIu64 " in found.000)%s%s\n", res.repaired, res.entries_removed,
                res.orphans_relinked, res.orphans_found, res.mirror_repaired ? ", $MFTMirr rewritten" : "",
                res.repair_ran || !res.problems ? "" : " (not run: some problems cannot be repaired here)");
    }
    fprintf(out, "remaining:    %" PRIu64 "\n", res.remaining);
    if (res.dirty_cleared)
        fprintf(out, "dirty mark:   cleared\n");
    else if (res.state == UE_VOL_DIRTY)
        fprintf(out, "dirty mark:   kept\n");
    return code;
}
