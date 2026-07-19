// Pure decision for the Linux AppImage updater, split into its own module
// (no electron/fs/network imports) so the download-and-overwrite-executable
// trigger has a standalone truth-table test — see
// tests/linux-update-decision.test.js. update-manager.ts's checkNowLinux()
// calls this to choose between: the running build is already current, the
// target nightly is already staged this session, or it must be downloaded
// and swapped in.

export type LinuxUpdateDecision = 'idle' | 'staged' | 'download';

/**
 * Whether a GitHub release's `target_commitish` is a real commit SHA rather
 * than a branch name. GitHub sets it to whatever the release was published
 * against: a full 40-char hex SHA only if the nightly pipeline pinned one,
 * otherwise a branch name like "main". `linuxUpdateDecision()` compares it
 * SHA-vs-SHA, so a branch name would never match the baked SHA and would send
 * every check to 'download' — re-fetching the ~1.5GB AppImage forever and
 * never reaching 'idle'. checkNowLinux() gates on this and fails safe instead.
 */
export function isCommitSha(value: string | null | undefined): boolean {
    return typeof value === 'string' && /^[0-9a-f]{40}$/i.test(value);
}

/**
 * @param bakedSha    commit the running build was cut from (build-info.json),
 *                    or null for a dev/unknown build.
 * @param remoteSha   target_commitish of the latest nightly GitHub release.
 * @param downloadedSha  remote SHA already fetched + staged this session, else null.
 */
export function linuxUpdateDecision(
    bakedSha: string | null,
    remoteSha: string,
    downloadedSha: string | null,
): LinuxUpdateDecision {
    // Running build IS the published nightly (baked commit matches release).
    if (bakedSha && bakedSha === remoteSha) return 'idle';
    // Already fetched + staged this exact nightly earlier this session, so
    // don't re-download the ~1.5GB AppImage on the next poll.
    if (downloadedSha === remoteSha) return 'staged';
    // Running build differs (older, or an unknown/dev build with no baked
    // SHA) and we haven't staged this nightly yet → fetch + swap it in.
    return 'download';
}
