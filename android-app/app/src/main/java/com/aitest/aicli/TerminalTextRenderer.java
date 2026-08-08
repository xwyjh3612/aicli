package com.aitest.aicli;

import android.text.Editable;
import android.view.ViewTreeObserver;
import android.widget.ScrollView;
import android.widget.TextView;

/**
 * Applies terminal snapshots without replacing the complete TextView buffer.
 * Codex repaints its TUI frequently while thinking; replacing the full text on
 * every snapshot forces Android to rebuild and redraw the whole layout.
 */
final class TerminalTextRenderer {
    private static final int MAX_CHARACTERS = 64 * 1024;
    private static final int RETAIN_CHARACTERS = 48 * 1024;

    private final TextView output;
    private final ScrollView scroll;
    private final Editable buffer;
    private String renderedText = "";
    private String pendingSnapshot;
    private boolean snapshotFramePosted;
    private boolean scrollPosted;

    TerminalTextRenderer(TextView output, ScrollView scroll) {
        this.output = output;
        this.scroll = scroll;
        output.setText("", TextView.BufferType.EDITABLE);
        Editable editable = output.getEditableText();
        if (editable == null) {
            throw new IllegalStateException("terminal output requires an editable text buffer");
        }
        buffer = editable;
    }

    void clear() {
        pendingSnapshot = null;
        applyText("", false);
        scroll.scrollTo(0, 0);
    }

    void appendLine(String line) {
        if (line == null || line.isEmpty()) return;
        String base = pendingSnapshot != null ? pendingSnapshot : renderedText;
        StringBuilder next = new StringBuilder(base.length() + line.length() + 1);
        next.append(base);
        if (next.length() > 0) next.append('\n');
        next.append(line);
        if (next.length() > MAX_CHARACTERS) {
            int start = next.length() - RETAIN_CHARACTERS;
            int lineBreak = next.indexOf("\n", start);
            next.delete(0, lineBreak >= 0 ? lineBreak + 1 : start);
        }
        pendingSnapshot = null;
        applyText(next.toString(), true);
    }

    void submitSnapshot(String text) {
        String next = text == null ? "" : text;
        if (next.equals(pendingSnapshot)) return;
        if (pendingSnapshot == null && next.equals(renderedText)) return;
        pendingSnapshot = next;
        if (snapshotFramePosted) return;
        snapshotFramePosted = true;
        output.postOnAnimation(this::drainSnapshot);
    }

    private void drainSnapshot() {
        snapshotFramePosted = false;
        String next = pendingSnapshot;
        pendingSnapshot = null;
        if (next != null) applyText(next, true);
        // If a snapshot arrived from another callback while this frame was
        // being applied, publish only the newest value on the next frame.
        if (pendingSnapshot != null && !snapshotFramePosted) {
            snapshotFramePosted = true;
            output.postOnAnimation(this::drainSnapshot);
        }
    }

    private void applyText(String next, boolean followOutput) {
        String previous = renderedText;
        if (next.equals(previous)) return;
        boolean wasNearBottom = followOutput && isNearBottom();

        int prefix = commonPrefix(previous, next);
        int suffix = commonSuffix(previous, next, prefix);
        int previousEnd = previous.length() - suffix;
        int nextEnd = next.length() - suffix;

        // Keep the TextView's existing buffer and replace only the changed
        // range. Unchanged transcript rows retain their layout and no longer
        // flash whenever Codex updates Working/model/composer rows.
        buffer.replace(prefix, previousEnd, next, prefix, nextEnd);
        renderedText = next;
        if (wasNearBottom) requestScrollToBottom();
    }

    private static int commonPrefix(String left, String right) {
        int limit = Math.min(left.length(), right.length());
        int index = 0;
        while (index < limit && left.charAt(index) == right.charAt(index)) index++;
        if (index > 0 && index < left.length() && index < right.length()
                && Character.isHighSurrogate(left.charAt(index - 1))) {
            index--;
        }
        return index;
    }

    private static int commonSuffix(String left, String right, int prefix) {
        int leftIndex = left.length();
        int rightIndex = right.length();
        int suffix = 0;
        while (leftIndex - suffix > prefix && rightIndex - suffix > prefix
                && left.charAt(leftIndex - suffix - 1)
                == right.charAt(rightIndex - suffix - 1)) {
            suffix++;
        }
        return suffix;
    }

    private boolean isNearBottom() {
        int viewport = Math.max(0, scroll.getHeight()
                - scroll.getPaddingTop() - scroll.getPaddingBottom());
        int bottom = Math.max(0, output.getHeight() - viewport);
        int threshold = Math.round(48 * output.getResources().getDisplayMetrics().density);
        return bottom - scroll.getScrollY() <= threshold;
    }

    private void requestScrollToBottom() {
        if (scrollPosted) return;
        scrollPosted = true;
        scroll.getViewTreeObserver().addOnPreDrawListener(
                new ViewTreeObserver.OnPreDrawListener() {
            @Override public boolean onPreDraw() {
                ViewTreeObserver observer = scroll.getViewTreeObserver();
                if (observer.isAlive()) observer.removeOnPreDrawListener(this);
                scrollPosted = false;
                int viewport = Math.max(0, scroll.getHeight()
                        - scroll.getPaddingTop() - scroll.getPaddingBottom());
                int target = Math.max(0, output.getHeight() - viewport);
                if (Math.abs(scroll.getScrollY() - target) > 1) {
                    scroll.scrollTo(0, target);
                }
                return true;
            }
        });
        scroll.invalidate();
    }
}
