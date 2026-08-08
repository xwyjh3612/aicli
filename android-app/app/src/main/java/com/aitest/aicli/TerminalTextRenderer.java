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
        view.setTerminal(state.terminal, 0, 0, true);
        return StreamResult.APPLIED;
    }

    synchronized StreamResult data(String terminalSessionId, long sequence, String data) {
        SessionState state = selectedState();
        StreamResult validation = validate(state, terminalSessionId, sequence);
        if (validation != StreamResult.APPLIED) return validation;
        boolean follow = view.isNearBottom();
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
        boolean follow = view.isNearBottom();
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
