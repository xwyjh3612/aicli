package com.aitest.aicli;

import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * Owns one VT screen state per recently visited project and attaches the
 * selected state to the native TerminalView.
 */
final class TerminalTextRenderer {
    enum StreamResult { APPLIED, IGNORED, GAP }

    private static final int MAX_CACHED_PROJECTS = 6;
    private static final long SYNCHRONIZED_REFRESH_DELAY_MS = 180L;

    private static final class SessionState {
        final VtTerminalEmulator terminal = new VtTerminalEmulator(32, 120);
        String terminalSessionId = "";
        long sequence;
        int scrollX;
        int scrollY;
        boolean followBottom = true;
        String historyToken = "";
        int historyStartLine;
        boolean historyHasMore;
        boolean historyLoading;
    }

    static final class HistoryRequest {
        final String projectId;
        final String historyToken;
        final int beforeLine;

        HistoryRequest(String projectId, String historyToken, int beforeLine) {
            this.projectId = projectId;
            this.historyToken = historyToken;
            this.beforeLine = beforeLine;
        }
    }

    private final TerminalView view;
    private final LinkedHashMap<String, SessionState> sessions =
            new LinkedHashMap<>(8, 0.75f, true);
    private String selectedProjectId = "";
    private boolean synchronizedRefreshPosted;
    private final Runnable synchronizedRefresh = this::flushSynchronizedOutput;

    TerminalTextRenderer(TerminalView view) {
        this.view = view;
    }

    synchronized void selectSession(String projectId) {
        cancelSynchronizedRefresh();
        saveViewport();
        selectedProjectId = projectId == null ? "" : projectId;
        if (selectedProjectId.isEmpty()) {
            view.setTerminal(null, 0, 0, true);
            return;
        }
        SessionState state = state(selectedProjectId);
        view.setTerminal(state.terminal, state.scrollX, state.scrollY, state.followBottom);
        trimCache();
    }

    synchronized void restoreSelectedSession(String projectId) {
        if (projectId == null || projectId.isEmpty() ||
                !projectId.equals(selectedProjectId)) return;
        saveViewport();
        SessionState state = sessions.get(projectId);
        if (state == null) return;
        view.setTerminal(state.terminal, state.scrollX, state.scrollY,
                state.followBottom);
    }

    synchronized void detach() {
        cancelSynchronizedRefresh();
        saveViewport();
        selectedProjectId = "";
        view.setTerminal(null, 0, 0, true);
    }

    synchronized void clearAll() {
        cancelSynchronizedRefresh();
        sessions.clear();
        selectedProjectId = "";
        view.setTerminal(null, 0, 0, true);
    }

    synchronized String currentTerminalSessionId() {
        SessionState state = sessions.get(selectedProjectId);
        return state == null ? "" : state.terminalSessionId;
    }

    synchronized long currentSequence() {
        SessionState state = sessions.get(selectedProjectId);
        return state == null ? 0L : state.sequence;
    }

    synchronized void followBottom() {
        SessionState state = sessions.get(selectedProjectId);
        if (state == null) return;
        state.followBottom = true;
        view.scrollToBottom();
    }

    synchronized void releaseFollowBottom() {
        SessionState state = sessions.get(selectedProjectId);
        if (state != null) state.followBottom = false;
    }

    synchronized StreamResult reset(String terminalSessionId, long baseSequence,
                                    int rows, int columns) {
        SessionState state = selectedState();
        if (state == null || terminalSessionId == null || terminalSessionId.isEmpty()) {
            return StreamResult.IGNORED;
        }
        if (terminalSessionId.equals(state.terminalSessionId) &&
                state.sequence > baseSequence) {
            return StreamResult.IGNORED;
        }
        state.terminalSessionId = terminalSessionId;
        state.sequence = Math.max(0L, baseSequence);
        state.terminal.reset(rows, columns);
        state.scrollX = 0;
        state.scrollY = 0;
        state.followBottom = true;
        state.historyToken = "";
        state.historyStartLine = 0;
        state.historyHasMore = false;
        state.historyLoading = false;
        view.setTerminal(state.terminal, 0, 0, true);
        return StreamResult.APPLIED;
    }

    synchronized StreamResult bootstrap(String terminalSessionId, long sequence,
                                        int rows, int columns, java.util.List<String> lines,
                                        String historyToken, int startLine,
                                        boolean hasMore) {
        SessionState state = selectedState();
        if (state == null || terminalSessionId == null || terminalSessionId.isEmpty()) {
            return StreamResult.IGNORED;
        }
        if (!terminalSessionId.equals(state.terminalSessionId)) return StreamResult.GAP;
        if (sequence < state.sequence) return StreamResult.IGNORED;
        state.terminal.loadPlainSnapshot(lines, rows, columns);
        state.sequence = Math.max(0L, sequence);
        state.scrollX = 0;
        state.scrollY = 0;
        state.followBottom = true;
        state.historyToken = historyToken == null ? "" : historyToken;
        state.historyStartLine = Math.max(0, startLine);
        state.historyHasMore = hasMore && state.historyStartLine > 0 &&
                !state.historyToken.isEmpty();
        state.historyLoading = false;
        view.setTerminal(state.terminal, 0, 0, true);
        return StreamResult.APPLIED;
    }

    synchronized HistoryRequest beginHistoryRequest() {
        SessionState state = sessions.get(selectedProjectId);
        if (state == null || state.historyLoading || !state.historyHasMore ||
                state.historyStartLine <= 0 || state.historyToken.isEmpty()) return null;
        state.historyLoading = true;
        return new HistoryRequest(selectedProjectId, state.historyToken,
                state.historyStartLine);
    }

    synchronized void historyLoaded(String projectId, String historyToken,
                                    int startLine, java.util.List<String> lines,
                                    boolean hasMore, boolean expired) {
        SessionState state = sessions.get(projectId);
        if (state == null || !state.historyToken.equals(historyToken)) return;
        state.historyLoading = false;
        if (expired || lines == null || lines.isEmpty()) {
            state.historyHasMore = false;
            return;
        }
        boolean selected = projectId.equals(selectedProjectId);
        int previousHeight = selected ? view.captureHistoryContentHeight() : 0;
        int previousScrollY = selected ? view.getScrollY() : 0;
        int added = state.terminal.prependPlainHistory(lines);
        int skipped = Math.max(0, lines.size() - added);
        state.historyStartLine = Math.max(0, startLine + skipped);
        state.historyHasMore = added > 0 && hasMore && state.historyStartLine > 0;
        if (selected && added > 0) {
            view.terminalHistoryPrepended(previousHeight, previousScrollY);
        }
    }

    synchronized void historyFailed(String projectId, String historyToken) {
        SessionState state = sessions.get(projectId);
        if (state != null && state.historyToken.equals(historyToken)) {
            state.historyLoading = false;
        }
    }

    synchronized StreamResult data(String terminalSessionId, long sequence, String data) {
        SessionState state = selectedState();
        StreamResult validation = validate(state, terminalSessionId, sequence);
        if (validation != StreamResult.APPLIED) return validation;
        // DECSET 2026 lets Claude build one atomic screen repaint across
        // several terminal.data frames. During that interval the emulator's
        // content height changes before TerminalView is allowed to draw and
        // scroll. Re-reading isNearBottom() for the later frames therefore
        // mistakes the temporarily stale viewport for a deliberate user
        // scroll and leaves the completed answer below the visible area.
        // Capture the user's follow intent when the synchronized frame begins
        // and preserve it until DECRST 2026 closes the frame.
        boolean wasSynchronized = state.terminal.synchronizedOutput();
        boolean follow = wasSynchronized ? state.followBottom : view.isNearBottom();
        state.terminal.feed(data == null ? "" : data);
        state.sequence = sequence;
        state.followBottom = follow;
        if (state.terminal.synchronizedOutput()) {
            scheduleSynchronizedRefresh();
        } else {
            cancelSynchronizedRefresh();
            view.terminalChanged(follow);
        }
        return StreamResult.APPLIED;
    }

    synchronized StreamResult resize(String terminalSessionId, long sequence,
                                      int rows, int columns) {
        SessionState state = selectedState();
        StreamResult validation = validate(state, terminalSessionId, sequence);
        if (validation != StreamResult.APPLIED) return validation;
        boolean follow = state.terminal.synchronizedOutput()
                ? state.followBottom : view.isNearBottom();
        state.terminal.resize(rows, columns);
        state.sequence = sequence;
        state.followBottom = follow;
        view.terminalChanged(follow);
        return StreamResult.APPLIED;
    }

    synchronized void appendLine(String line) {
        SessionState state = selectedState();
        if (state == null || line == null || line.isEmpty()) return;
        boolean follow = view.isNearBottom();
        state.terminal.feed("\r\n" + line + "\r\n");
        state.followBottom = follow;
        view.terminalChanged(follow);
    }

    /**
     * Claude and other TUIs use DECSET 2026 to apply a screen update as one
     * frame. A dropped or delayed closing sequence must not leave the Android
     * view frozen indefinitely. Keep the normal atomic refresh, but cap its
     * latency just like a native terminal renderer does.
     */
    private synchronized void scheduleSynchronizedRefresh() {
        if (synchronizedRefreshPosted) return;
        synchronizedRefreshPosted = true;
        view.postDelayed(synchronizedRefresh, SYNCHRONIZED_REFRESH_DELAY_MS);
    }

    private void flushSynchronizedOutput() {
        boolean follow;
        synchronized (this) {
            synchronizedRefreshPosted = false;
            SessionState state = sessions.get(selectedProjectId);
            if (state == null) return;
            follow = state.followBottom;
        }
        view.terminalChanged(follow);
    }

    private synchronized void cancelSynchronizedRefresh() {
        if (!synchronizedRefreshPosted) return;
        synchronizedRefreshPosted = false;
        view.removeCallbacks(synchronizedRefresh);
    }

    private StreamResult validate(SessionState state, String sessionId, long sequence) {
        if (state == null || sessionId == null || sessionId.isEmpty() || sequence <= 0) {
            return StreamResult.IGNORED;
        }
        if (!sessionId.equals(state.terminalSessionId)) return StreamResult.GAP;
        if (sequence <= state.sequence) return StreamResult.IGNORED;
        return sequence == state.sequence + 1 ? StreamResult.APPLIED : StreamResult.GAP;
    }

    private SessionState selectedState() {
        return selectedProjectId.isEmpty() ? null : state(selectedProjectId);
    }

    private SessionState state(String projectId) {
        SessionState value = sessions.get(projectId);
        if (value == null) {
            value = new SessionState();
            sessions.put(projectId, value);
        }
        return value;
    }

    private void saveViewport() {
        SessionState current = sessions.get(selectedProjectId);
        if (current == null) return;
        current.scrollX = view.getScrollX();
        current.scrollY = view.getScrollY();
        current.followBottom = view.followsBottom();
    }

    private void trimCache() {
        if (sessions.size() <= MAX_CACHED_PROJECTS) return;
        Iterator<Map.Entry<String, SessionState>> iterator = sessions.entrySet().iterator();
        while (sessions.size() > MAX_CACHED_PROJECTS && iterator.hasNext()) {
            Map.Entry<String, SessionState> entry = iterator.next();
            if (!entry.getKey().equals(selectedProjectId)) iterator.remove();
        }
    }
}
