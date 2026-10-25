package com.aitest.aicli;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.util.AttributeSet;
import android.view.ActionMode;
import android.view.GestureDetector;
import android.view.Menu;
import android.view.MenuItem;
import android.view.MotionEvent;
import android.view.ScaleGestureDetector;
import android.view.View;
import android.widget.OverScroller;

import androidx.annotation.Nullable;

import java.util.ArrayList;

/** Native cell renderer with terminal-style scrolling and pinch zoom. */
public final class TerminalView extends View {
    private static final float MIN_TEXT_SP = 7.5f;
    private static final float MAX_TEXT_SP = 19f;
    private static final int MENU_COPY = 1;
    private static final int MENU_CANCEL = 2;
    private static final int HANDLE_NONE = 0;
    private static final int HANDLE_START = 1;
    private static final int HANDLE_END = 2;

    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.SUBPIXEL_TEXT_FLAG);
    private final Paint fillPaint = new Paint();
    private final Paint selectionPaint = new Paint();
    private final Paint selectionHandlePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint cursorPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final GestureDetector gestures;
    private final ScaleGestureDetector scaleGestures;
    private final OverScroller scroller;
    private final float density;
    private final float scaledDensity;
    private final ArrayList<VisualLine> visualLines = new ArrayList<>();

    private VtTerminalEmulator terminal;
    private float textSizeSp = 12f;
    private float cellWidth;
    private float cellHeight;
    private float baselineOffset;
    private boolean framePosted;
    private boolean followOnFrame;
    private boolean cursorOn = true;
    private boolean attached;
    private boolean softWrapEnabled = true;
    private long visualRevision = Long.MIN_VALUE;
    private int visualWrapColumns = -1;
    private int visualLogicalLines = -1;
    private int[] visualStarts = new int[0];
    private HistoryLoadListener historyLoadListener;
    private UserScrollListener userScrollListener;
    private boolean selecting;
    private SelectionPoint selectionAnchor;
    private SelectionPoint selectionFocus;
    private SelectionPoint selectionPivotStart;
    private SelectionPoint selectionPivotEnd;
    private ActionMode selectionActionMode;
    private int draggingSelectionHandle = HANDLE_NONE;
    private float selectionHandleRadius;

    interface HistoryLoadListener {
        void onHistoryLoadRequested();
    }

    interface UserScrollListener {
        /** True only while the user gesture is following the bottom. */
        void onUserScroll(boolean followsBottom);
    }

    private static final class VisualLine {
        final int logicalRow;
        final int startColumn;
        final int endColumn;

        VisualLine(int logicalRow, int startColumn, int endColumn) {
            this.logicalRow = logicalRow;
            this.startColumn = startColumn;
            this.endColumn = endColumn;
        }
    }

    private static final class SelectionPoint {
        final int row;
        final int column;

        SelectionPoint(int row, int column) {
            this.row = row;
            this.column = column;
        }
    }

    private static final class SelectionRange {
        final SelectionPoint start;
        final SelectionPoint end;

        SelectionRange(SelectionPoint start, SelectionPoint end) {
            this.start = start;
            this.end = end;
        }
    }

    private static final class HandlePosition {
        final float x;
        final float lineBottom;
        final float centerY;

        HandlePosition(float x, float lineBottom, float centerY) {
            this.x = x;
            this.lineBottom = lineBottom;
            this.centerY = centerY;
        }
    }

    private final Runnable cursorBlink = new Runnable() {
        @Override public void run() {
            if (!attached) return;
            cursorOn = !cursorOn;
            invalidate();
            postDelayed(this, 550L);
        }
    };

    public TerminalView(Context context) {
        this(context, null);
    }

    public TerminalView(Context context, @Nullable AttributeSet attributes) {
        this(context, attributes, 0);
    }

    public TerminalView(Context context, @Nullable AttributeSet attributes, int styleAttribute) {
        super(context, attributes, styleAttribute);
        density = getResources().getDisplayMetrics().density;
        scaledDensity = getResources().getDisplayMetrics().scaledDensity;
        scroller = new OverScroller(context);
        textPaint.setTypeface(Typeface.MONOSPACE);
        cursorPaint.setStyle(Paint.Style.STROKE);
        cursorPaint.setStrokeWidth(Math.max(1f, density));
        cursorPaint.setColor(Color.argb(220, 235, 235, 235));
        selectionPaint.setStyle(Paint.Style.FILL);
        selectionPaint.setColor(Color.argb(150, 58, 128, 246));
        selectionHandlePaint.setStyle(Paint.Style.FILL);
        selectionHandlePaint.setColor(getResources().getColor(
                R.color.aicli_primary, context.getTheme()));
        selectionHandleRadius = 7f * density;
        updateMetrics();

        setFocusable(true);
        setFocusableInTouchMode(true);
        setVerticalScrollBarEnabled(true);
        setHorizontalScrollBarEnabled(true);
        setScrollbarFadingEnabled(false);
        setScrollBarStyle(SCROLLBARS_INSIDE_OVERLAY);
        setOverScrollMode(OVER_SCROLL_NEVER);

        gestures = new GestureDetector(context, new GestureDetector.SimpleOnGestureListener() {
            @Override public boolean onDown(MotionEvent event) {
                if (!scroller.isFinished()) scroller.forceFinished(true);
                if (userScrollListener != null) userScrollListener.onUserScroll(false);
                return true;
            }

            @Override public boolean onSingleTapUp(MotionEvent event) {
                if (selecting) return true;
                // A tap is not a scroll gesture. If it began at the bottom,
                // resume following new output once the tap finishes.
                notifyUserScrollPosition();
                performClick();
                return true;
            }

            @Override public void onLongPress(MotionEvent event) {
                if (terminal == null) return;
                SelectionPoint point = pointAt(event.getX(), event.getY());
                if (point == null) return;
                beginSelection(point);
            }

            @Override public boolean onScroll(MotionEvent first, MotionEvent current,
                                              float distanceX, float distanceY) {
                if (selecting) {
                    updateSelection(current.getX(), current.getY());
                    return true;
                }
                scrollByClamped(Math.round(distanceX), Math.round(distanceY));
                notifyUserScrollPosition();
                if (distanceY < 0f) maybeRequestOlderHistory();
                return true;
            }

            @Override public boolean onFling(MotionEvent first, MotionEvent current,
                                             float velocityX, float velocityY) {
                if (selecting) return true;
                scroller.fling(getScrollX(), getScrollY(),
                        Math.round(-velocityX), Math.round(-velocityY),
                        0, maximumScrollX(), 0, maximumScrollY());
                postInvalidateOnAnimation();
                return true;
            }
        });
        scaleGestures = new ScaleGestureDetector(context,
                new ScaleGestureDetector.SimpleOnScaleGestureListener() {
            @Override public boolean onScale(ScaleGestureDetector detector) {
                float next = clamp(textSizeSp * detector.getScaleFactor(),
                        MIN_TEXT_SP, MAX_TEXT_SP);
                if (Math.abs(next - textSizeSp) < 0.02f) return true;
                float logicalX = (getScrollX() + detector.getFocusX() - getPaddingLeft()) /
                        Math.max(1f, cellWidth);
                float logicalY = (getScrollY() + detector.getFocusY() - getPaddingTop()) /
                        Math.max(1f, cellHeight);
                textSizeSp = next;
                updateMetrics();
                int targetX = Math.round(getPaddingLeft() + logicalX * cellWidth - detector.getFocusX());
                int targetY = Math.round(getPaddingTop() + logicalY * cellHeight - detector.getFocusY());
                scrollToClamped(targetX, targetY);
                invalidate();
                awakenScrollBars();
                return true;
            }
        });
    }

    void setTerminal(@Nullable VtTerminalEmulator value, int restoredX,
                     int restoredY, boolean followBottom) {
        if (terminal != value) clearSelection();
        terminal = value;
        invalidateVisualLayout();
        scroller.forceFinished(true);
        if (value == null) {
            scrollTo(0, 0);
            invalidate();
            return;
        }
        if (followBottom) {
            post(() -> scrollToClamped(restoredX, maximumScrollY()));
        } else {
            post(() -> scrollToClamped(restoredX, restoredY));
        }
        invalidate();
    }

    void terminalChanged(boolean followBottom) {
        followOnFrame |= followBottom;
        if (framePosted) return;
        framePosted = true;
        postOnAnimation(() -> {
            framePosted = false;
            boolean follow = followOnFrame;
            followOnFrame = false;
            invalidateVisualLayout();
            if (follow) scrollToClamped(getScrollX(), maximumScrollY());
            else scrollToClamped(getScrollX(), getScrollY());
            invalidate();
            awakenScrollBars();
        });
    }

    void setHistoryLoadListener(@Nullable HistoryLoadListener listener) {
        historyLoadListener = listener;
    }

    void setUserScrollListener(@Nullable UserScrollListener listener) {
        userScrollListener = listener;
    }

    int captureHistoryContentHeight() {
        return contentHeight();
    }

    void terminalHistoryPrepended(int previousContentHeight, int previousScrollY) {
        invalidateVisualLayout();
        int nextHeight = contentHeight();
        scrollToClamped(getScrollX(), previousScrollY +
                Math.max(0, nextHeight - previousContentHeight));
        invalidate();
        awakenScrollBars();
    }

    boolean isNearBottom() {
        return maximumScrollY() - getScrollY() <= Math.max(cellHeight * 2f, 32f * density);
    }

    boolean followsBottom() {
        return isNearBottom();
    }

    void scrollToBottom() {
        if (!scroller.isFinished()) scroller.forceFinished(true);
        scrollToClamped(softWrapEnabled ? 0 : getScrollX(), maximumScrollY());
        invalidate();
        awakenScrollBars();
    }

    public void setSoftWrapEnabled(boolean enabled) {
        if (softWrapEnabled == enabled) return;
        boolean follow = isNearBottom();
        int logicalTop = logicalRowAtViewportTop();
        softWrapEnabled = enabled;
        invalidateVisualLayout();
        post(() -> {
            ensureVisualLayout();
            int targetY = follow ? maximumScrollY()
                    : Math.round(getPaddingTop() + visualStartForLogicalRow(logicalTop) * cellHeight);
            scrollToClamped(enabled ? 0 : getScrollX(), targetY);
            invalidate();
            awakenScrollBars();
        });
    }

    public boolean softWrapEnabled() {
        return softWrapEnabled;
    }

    @Override public boolean performClick() {
        super.performClick();
        requestFocus();
        return true;
    }

    @Override public boolean onTouchEvent(MotionEvent event) {
        if (selecting) {
            int action = event.getActionMasked();
            if (action == MotionEvent.ACTION_DOWN) {
                draggingSelectionHandle = hitSelectionHandle(event.getX(), event.getY());
                if (draggingSelectionHandle != HANDLE_NONE) {
                    getParent().requestDisallowInterceptTouchEvent(true);
                    return true;
                }
            } else if (action == MotionEvent.ACTION_MOVE &&
                    draggingSelectionHandle != HANDLE_NONE) {
                updateSelectionHandle(event.getX(), event.getY());
                return true;
            } else if ((action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) &&
                    draggingSelectionHandle != HANDLE_NONE) {
                if (action == MotionEvent.ACTION_UP) {
                    updateSelectionHandle(event.getX(), event.getY());
                }
                draggingSelectionHandle = HANDLE_NONE;
                getParent().requestDisallowInterceptTouchEvent(false);
                return true;
            }
        }
        boolean scaled = scaleGestures.onTouchEvent(event);
        boolean gestured = !scaleGestures.isInProgress() && gestures.onTouchEvent(event);
        return scaled || gestured || super.onTouchEvent(event);
    }

    @Override protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        attached = true;
        removeCallbacks(cursorBlink);
        postDelayed(cursorBlink, 550L);
    }

    @Override protected void onDetachedFromWindow() {
        attached = false;
        removeCallbacks(cursorBlink);
        clearSelection();
        super.onDetachedFromWindow();
    }

    @Override protected void onSizeChanged(int width, int height, int oldWidth, int oldHeight) {
        boolean follow = terminal != null && isNearBottom();
        super.onSizeChanged(width, height, oldWidth, oldHeight);
        invalidateVisualLayout();
        if (follow) post(() -> scrollToClamped(softWrapEnabled ? 0 : getScrollX(), maximumScrollY()));
        else scrollToClamped(softWrapEnabled ? 0 : getScrollX(), getScrollY());
    }

    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        canvas.drawColor(VtTerminalEmulator.DEFAULT_BACKGROUND);
        VtTerminalEmulator model = terminal;
        if (model == null) return;
        ensureVisualLayout();

        int firstRow = Math.max(0, (int) Math.floor(
                (getScrollY() - getPaddingTop()) / Math.max(1f, cellHeight)));
        int visibleRows = (int) Math.ceil(getHeight() / Math.max(1f, cellHeight)) + 2;
        int lastRow = Math.min(visualLines.size(), firstRow + visibleRows);
        int viewportFirstColumn = softWrapEnabled ? 0 : Math.max(0, (int) Math.floor(
                (getScrollX() - getPaddingLeft()) / Math.max(1f, cellWidth)));
        int visibleColumns = (int) Math.ceil(getWidth() / Math.max(1f, cellWidth)) + 2;

        for (int row = firstRow; row < lastRow; row++) {
            VisualLine visual = visualLines.get(row);
            VtTerminalEmulator.Line line = model.lineAt(visual.logicalRow);
            if (line == null) continue;
            int firstColumn = softWrapEnabled ? visual.startColumn : viewportFirstColumn;
            int lastColumn = softWrapEnabled ? visual.endColumn
                    : Math.min(line.columns(), firstColumn + visibleColumns);
            float top = getPaddingTop() + row * cellHeight;
            drawLine(canvas, line, visual.logicalRow, firstColumn, lastColumn, top,
                    softWrapEnabled ? visual.startColumn : 0);
        }

        if (model.cursorVisible() && cursorOn) {
            int logicalCursorRow = model.cursorDisplayRow();
            int cursorRow = visualStartForLogicalRow(logicalCursorRow);
            int cursorColumn = model.cursorColumn();
            if (softWrapEnabled) {
                int columns = Math.max(1, currentWrapColumns());
                cursorRow += cursorColumn / columns;
                cursorColumn %= columns;
            }
            if (cursorRow >= firstRow && cursorRow < lastRow &&
                    (softWrapEnabled || (cursorColumn >= viewportFirstColumn &&
                            cursorColumn < viewportFirstColumn + visibleColumns))) {
                float left = getPaddingLeft() + cursorColumn * cellWidth;
                float top = getPaddingTop() + cursorRow * cellHeight;
                canvas.drawRect(left + 0.5f, top + 0.5f,
                        left + cellWidth - 0.5f, top + cellHeight - 0.5f, cursorPaint);
            }
        }
        drawSelectionHandles(canvas);
    }

    private void drawLine(Canvas canvas, VtTerminalEmulator.Line line, int logicalRow,
                          int firstColumn, int lastColumn, float top,
                          int originColumn) {
        for (int column = firstColumn; column < lastColumn;) {
            long colors = line.colorsAt(column);
            byte flags = line.flagsAt(column);
            int runEnd = column + 1;
            while (runEnd < lastColumn && line.colorsAt(runEnd) == colors &&
                    line.flagsAt(runEnd) == flags) runEnd++;
            int foreground = VtTerminalEmulator.foreground(colors);
            int background = VtTerminalEmulator.background(colors);
            if ((flags & VtTerminalEmulator.STYLE_INVERSE) != 0) {
                int swap = foreground;
                foreground = background;
                background = swap;
            }
            if (background != VtTerminalEmulator.DEFAULT_BACKGROUND) {
                fillPaint.setColor(background);
                canvas.drawRect(getPaddingLeft() + (column - originColumn) * cellWidth, top,
                        getPaddingLeft() + (runEnd - originColumn) * cellWidth,
                        top + cellHeight, fillPaint);
            }
            configureTextPaint(foreground, background, flags);
            for (int cell = column; cell < runEnd; cell++) {
                if (isCellSelected(logicalRow, cell)) {
                    selectionPaint.setColor(Color.argb(170, 58, 128, 246));
                    canvas.drawRect(
                            getPaddingLeft() + (cell - originColumn) * cellWidth,
                            top,
                            getPaddingLeft() + (cell + 1 - originColumn) * cellWidth,
                            top + cellHeight,
                            selectionPaint);
                }
                int codePoint = line.codePointAt(cell);
                if (codePoint <= 0 || codePoint == ' ') continue;
                String glyph = new String(Character.toChars(codePoint));
                canvas.drawText(glyph, getPaddingLeft() + (cell - originColumn) * cellWidth,
                        top + baselineOffset, textPaint);
            }
            column = runEnd;
        }
    }

    private void beginSelection(SelectionPoint point) {
        clearSelection();
        SelectionRange range = wordRangeAt(point);
        if (range == null) return;
        selecting = true;
        selectionAnchor = range.start;
        selectionFocus = range.end;
        selectionPivotStart = range.start;
        selectionPivotEnd = range.end;
        performHapticFeedback(android.view.HapticFeedbackConstants.LONG_PRESS);
        selectionActionMode = startActionMode(new SelectionActionMode(),
                ActionMode.TYPE_FLOATING);
        invalidate();
    }

    private void updateSelection(float x, float y) {
        if (!selecting || selectionPivotStart == null || selectionPivotEnd == null) return;
        SelectionPoint point = pointAt(x, y);
        if (point == null) return;
        SelectionRange range = wordRangeAt(point);
        if (range == null) return;
        if (comparePoints(point, selectionPivotStart) <= 0) {
            selectionAnchor = range.start;
            selectionFocus = selectionPivotEnd;
        } else {
            selectionAnchor = selectionPivotStart;
            selectionFocus = range.end;
        }
        invalidate();
    }

    private void drawSelectionHandles(Canvas canvas) {
        SelectionRange range = orderedSelectionRange();
        if (range == null) return;
        HandlePosition start = handlePosition(range.start, false);
        HandlePosition end = handlePosition(range.end, true);
        if (start != null) drawSelectionHandle(canvas, start);
        if (end != null) drawSelectionHandle(canvas, end);
    }

    private void drawSelectionHandle(Canvas canvas, HandlePosition position) {
        float stemHalfWidth = Math.max(1f, density);
        float stemBottom = position.centerY - selectionHandleRadius * 0.55f;
        canvas.drawRect(position.x - stemHalfWidth, position.lineBottom,
                position.x + stemHalfWidth, stemBottom, selectionHandlePaint);
        canvas.drawCircle(position.x, position.centerY,
                selectionHandleRadius, selectionHandlePaint);
    }

    private int hitSelectionHandle(float x, float y) {
        SelectionRange range = orderedSelectionRange();
        if (range == null) return HANDLE_NONE;
        float hitRadius = selectionHandleRadius + 12f * density;
        HandlePosition start = handlePosition(range.start, false);
        HandlePosition end = handlePosition(range.end, true);
        if (end != null && isInsideHandle(end, x, y, hitRadius)) return HANDLE_END;
        if (start != null && isInsideHandle(start, x, y, hitRadius)) return HANDLE_START;
        return HANDLE_NONE;
    }

    private boolean isInsideHandle(HandlePosition position, float x, float y, float radius) {
        float deltaX = x - (position.x - getScrollX());
        float deltaY = y - (position.centerY - getScrollY());
        return deltaX * deltaX + deltaY * deltaY <= radius * radius;
    }

    @Nullable
    private HandlePosition handlePosition(SelectionPoint point, boolean endHandle) {
        if (terminal == null || point.row < 0 || point.row >= terminal.totalLines()) return null;
        ensureVisualLayout();
        int firstVisual = visualStartForLogicalRow(point.row);
        int lastVisual = visualStartForLogicalRow(point.row + 1);
        if (firstVisual >= visualLines.size()) return null;
        int visualRow = firstVisual;
        for (int index = firstVisual; index < lastVisual && index < visualLines.size(); index++) {
            VisualLine visual = visualLines.get(index);
            if (point.column >= visual.startColumn && point.column < visual.endColumn) {
                visualRow = index;
                break;
            }
            visualRow = index;
        }
        VisualLine visual = visualLines.get(visualRow);
        int originColumn = softWrapEnabled ? visual.startColumn : 0;
        int displayColumn = clamp(point.column, visual.startColumn,
                Math.max(visual.startColumn, visual.endColumn - 1));
        float x = getPaddingLeft() +
                (displayColumn - originColumn + (endHandle ? 1f : 0f)) * cellWidth;
        float lineBottom = getPaddingTop() + (visualRow + 1f) * cellHeight;
        float centerY = lineBottom + 4f * density + selectionHandleRadius;
        return new HandlePosition(x, lineBottom, centerY);
    }

    private void updateSelectionHandle(float x, float y) {
        SelectionRange current = orderedSelectionRange();
        SelectionPoint touched = pointAt(x, y);
        if (current == null || touched == null) return;
        SelectionPoint touchedStart = glyphStartAt(touched);
        SelectionPoint touchedEnd = glyphEndAt(touched);
        if (draggingSelectionHandle == HANDLE_START) {
            if (comparePoints(touchedStart, current.end) <= 0) {
                selectionAnchor = touchedStart;
                selectionFocus = current.end;
            } else {
                selectionAnchor = glyphStartAt(current.end);
                selectionFocus = touchedEnd;
                draggingSelectionHandle = HANDLE_END;
            }
        } else if (draggingSelectionHandle == HANDLE_END) {
            if (comparePoints(touchedEnd, current.start) >= 0) {
                selectionAnchor = current.start;
                selectionFocus = touchedEnd;
            } else {
                selectionAnchor = touchedStart;
                selectionFocus = glyphEndAt(current.start);
                draggingSelectionHandle = HANDLE_START;
            }
        }
        SelectionRange updated = orderedSelectionRange();
        if (updated != null) {
            selectionPivotStart = updated.start;
            selectionPivotEnd = updated.end;
        }
        invalidate();
    }

    @Nullable
    private SelectionRange orderedSelectionRange() {
        if (!selecting || selectionAnchor == null || selectionFocus == null) return null;
        return comparePoints(selectionAnchor, selectionFocus) <= 0
                ? new SelectionRange(selectionAnchor, selectionFocus)
                : new SelectionRange(selectionFocus, selectionAnchor);
    }

    private SelectionPoint glyphStartAt(SelectionPoint point) {
        if (terminal == null) return point;
        VtTerminalEmulator.Line line = terminal.lineAt(point.row);
        if (line == null) return point;
        int column = TerminalWordBoundary.normalizeColumn(
                line.columns(), line::codePointAt, point.column);
        return new SelectionPoint(point.row, column);
    }

    private SelectionPoint glyphEndAt(SelectionPoint point) {
        SelectionPoint start = glyphStartAt(point);
        if (terminal == null) return start;
        VtTerminalEmulator.Line line = terminal.lineAt(start.row);
        if (line == null) return start;
        int end = start.column;
        while (end + 1 < line.columns() && line.codePointAt(end + 1) <= 0) end++;
        return new SelectionPoint(start.row, end);
    }

    @Nullable
    private SelectionRange wordRangeAt(SelectionPoint point) {
        if (terminal == null) return null;
        VtTerminalEmulator.Line line = terminal.lineAt(point.row);
        if (line == null) return null;
        TerminalWordBoundary.Range range = TerminalWordBoundary.find(
                line.columns(), line::codePointAt, point.column);
        return new SelectionRange(
                new SelectionPoint(point.row, range.start),
                new SelectionPoint(point.row, range.end));
    }

    private SelectionPoint pointAt(float x, float y) {
        if (terminal == null) return null;
        ensureVisualLayout();
        if (visualLines.isEmpty()) return null;
        int visualRow = clamp((int) Math.floor(
                (getScrollY() + y - getPaddingTop()) / Math.max(1f, cellHeight)),
                0, visualLines.size() - 1);
        VisualLine visual = visualLines.get(visualRow);
        VtTerminalEmulator.Line line = terminal.lineAt(visual.logicalRow);
        if (line == null) return null;
        int column = (int) Math.floor(
                (getScrollX() + x - getPaddingLeft()) / Math.max(1f, cellWidth));
        if (softWrapEnabled) column += visual.startColumn;
        column = clamp(column, 0, Math.max(0, line.columns() - 1));
        column = TerminalWordBoundary.normalizeColumn(
                line.columns(), line::codePointAt, column);
        return new SelectionPoint(visual.logicalRow, column);
    }

    private boolean isCellSelected(int row, int column) {
        if (!selecting || selectionAnchor == null || selectionFocus == null) return false;
        SelectionPoint start = selectionAnchor;
        SelectionPoint end = selectionFocus;
        if (comparePoints(start, end) > 0) {
            SelectionPoint swap = start;
            start = end;
            end = swap;
        }
        SelectionPoint current = new SelectionPoint(row, column);
        return comparePoints(current, start) >= 0 && comparePoints(current, end) <= 0;
    }

    private static int comparePoints(SelectionPoint first, SelectionPoint second) {
        if (first.row != second.row) return Integer.compare(first.row, second.row);
        return Integer.compare(first.column, second.column);
    }

    private String selectedText() {
        if (!selecting || terminal == null || selectionAnchor == null || selectionFocus == null) {
            return "";
        }
        SelectionPoint start = selectionAnchor;
        SelectionPoint end = selectionFocus;
        if (comparePoints(start, end) > 0) {
            SelectionPoint swap = start;
            start = end;
            end = swap;
        }
        StringBuilder result = new StringBuilder();
        for (int row = start.row; row <= end.row; row++) {
            VtTerminalEmulator.Line line = terminal.lineAt(row);
            if (line == null) continue;
            int first = row == start.row ? start.column : 0;
            int last = row == end.row ? end.column : line.usedColumns() - 1;
            first = clamp(first, 0, Math.max(0, line.columns() - 1));
            last = clamp(last, first, Math.max(first, line.columns() - 1));
            StringBuilder lineText = new StringBuilder();
            for (int column = first; column <= last; column++) {
                int codePoint = line.codePointAt(column);
                if (codePoint <= 0) continue;
                lineText.appendCodePoint(codePoint > 0 ? codePoint : ' ');
            }
            int length = lineText.length();
            while (length > 0 && lineText.charAt(length - 1) == ' ') length--;
            if (row > start.row) result.append('\n');
            result.append(lineText, 0, length);
        }
        return result.toString();
    }

    private void copySelection() {
        String text = selectedText();
        if (!text.isEmpty()) {
            ClipboardManager clipboard = (ClipboardManager) getContext().getSystemService(
                    Context.CLIPBOARD_SERVICE);
            if (clipboard != null) {
                clipboard.setPrimaryClip(ClipData.newPlainText("AiCli", text));
            }
        }
        if (selectionActionMode != null) selectionActionMode.finish();
    }

    private void clearSelection() {
        selecting = false;
        selectionAnchor = null;
        selectionFocus = null;
        selectionPivotStart = null;
        selectionPivotEnd = null;
        draggingSelectionHandle = HANDLE_NONE;
        if (selectionActionMode != null) {
            ActionMode mode = selectionActionMode;
            selectionActionMode = null;
            mode.finish();
        }
        invalidate();
    }

    private final class SelectionActionMode implements ActionMode.Callback {
        @Override public boolean onCreateActionMode(ActionMode mode, Menu menu) {
            menu.add(0, MENU_COPY, 0, "复制")
                    .setShowAsAction(MenuItem.SHOW_AS_ACTION_ALWAYS);
            menu.add(0, MENU_CANCEL, 1, "取消")
                    .setShowAsAction(MenuItem.SHOW_AS_ACTION_ALWAYS);
            return true;
        }

        @Override public boolean onPrepareActionMode(ActionMode mode, Menu menu) {
            return false;
        }

        @Override public boolean onActionItemClicked(ActionMode mode, MenuItem item) {
            if (item.getItemId() == MENU_COPY) {
                copySelection();
                return true;
            }
            if (item.getItemId() == MENU_CANCEL) {
                mode.finish();
                return true;
            }
            return false;
        }

        @Override public void onDestroyActionMode(ActionMode mode) {
            if (selectionActionMode == mode) selectionActionMode = null;
            selecting = false;
            selectionAnchor = null;
            selectionFocus = null;
            selectionPivotStart = null;
            selectionPivotEnd = null;
            draggingSelectionHandle = HANDLE_NONE;
            invalidate();
        }
    }

    private void configureTextPaint(int foreground, int background, byte flags) {
        if ((flags & VtTerminalEmulator.STYLE_HIDDEN) != 0) foreground = background;
        if ((flags & VtTerminalEmulator.STYLE_DIM) != 0) {
            foreground = blend(foreground, background, 0.56f);
        }
        textPaint.setColor(foreground);
        textPaint.setFakeBoldText((flags & VtTerminalEmulator.STYLE_BOLD) != 0);
        textPaint.setUnderlineText((flags & VtTerminalEmulator.STYLE_UNDERLINE) != 0);
        textPaint.setStrikeThruText((flags & VtTerminalEmulator.STYLE_STRIKE) != 0);
    }

    private void updateMetrics() {
        textPaint.setTextSize(textSizeSp * scaledDensity);
        Paint.FontMetrics metrics = textPaint.getFontMetrics();
        cellWidth = Math.max(1f, textPaint.measureText("M"));
        cellHeight = Math.max(1f, (float) Math.ceil(metrics.descent - metrics.ascent + density));
        baselineOffset = -metrics.ascent + density * 0.5f;
        invalidateVisualLayout();
    }

    private void scrollByClamped(int deltaX, int deltaY) {
        scrollToClamped(getScrollX() + deltaX, getScrollY() + deltaY);
        awakenScrollBars();
    }

    private void notifyUserScrollPosition() {
        UserScrollListener listener = userScrollListener;
        if (listener != null) listener.onUserScroll(isNearBottom());
    }

    private void maybeRequestOlderHistory() {
        HistoryLoadListener listener = historyLoadListener;
        if (listener == null) return;
        int threshold = Math.max(Math.round(cellHeight * 2f), Math.round(32f * density));
        if (getScrollY() <= threshold) listener.onHistoryLoadRequested();
    }

    private void scrollToClamped(int x, int y) {
        scrollTo(clamp(x, 0, maximumScrollX()), clamp(y, 0, maximumScrollY()));
    }

    private int contentWidth() {
        if (terminal == null || softWrapEnabled) return getWidth();
        return Math.round(
                getPaddingLeft() + terminal.columns() * cellWidth + getPaddingRight());
    }

    private int contentHeight() {
        if (terminal == null) return getHeight();
        ensureVisualLayout();
        return Math.round(getPaddingTop() + visualLines.size() * cellHeight + getPaddingBottom());
    }

    private int maximumScrollX() { return Math.max(0, contentWidth() - getWidth()); }
    private int maximumScrollY() { return Math.max(0, contentHeight() - getHeight()); }

    @Override protected int computeHorizontalScrollRange() { return contentWidth(); }
    @Override protected int computeHorizontalScrollExtent() { return getWidth(); }
    @Override protected int computeHorizontalScrollOffset() { return getScrollX(); }
    @Override protected int computeVerticalScrollRange() { return contentHeight(); }
    @Override protected int computeVerticalScrollExtent() { return getHeight(); }
    @Override protected int computeVerticalScrollOffset() { return getScrollY(); }

    @Override public void computeScroll() {
        if (!scroller.computeScrollOffset()) return;
        scrollToClamped(scroller.getCurrX(), scroller.getCurrY());
        notifyUserScrollPosition();
        postInvalidateOnAnimation();
    }

    private void ensureVisualLayout() {
        VtTerminalEmulator model = terminal;
        if (model == null) {
            visualLines.clear();
            visualStarts = new int[0];
            return;
        }
        int wrapColumns = softWrapEnabled ? currentWrapColumns() : model.columns();
        int logicalLines = model.totalLines();
        if (visualRevision == model.revision() && visualWrapColumns == wrapColumns &&
                visualLogicalLines == logicalLines) return;

        visualLines.clear();
        visualStarts = new int[logicalLines + 1];
        int cursorRow = model.cursorDisplayRow();
        int cursorColumn = model.cursorColumn();
        for (int row = 0; row < logicalLines; row++) {
            visualStarts[row] = visualLines.size();
            VtTerminalEmulator.Line line = model.lineAt(row);
            if (line == null) continue;
            if (!softWrapEnabled) {
                visualLines.add(new VisualLine(row, 0, line.columns()));
                continue;
            }
            int used = line.usedColumns();
            if (row == cursorRow) used = Math.max(used, cursorColumn + 1);
            // A desktop-width TUI separator should become one phone-width
            // separator, not four stacked stripes after visual reflow.
            if (line.isHorizontalRule()) used = Math.min(used, wrapColumns);
            used = Math.max(1, Math.min(line.columns(), used));
            int segments = Math.max(1, (used + wrapColumns - 1) / wrapColumns);
            for (int segment = 0; segment < segments; segment++) {
                int start = segment * wrapColumns;
                visualLines.add(new VisualLine(row, start,
                        Math.min(line.columns(), start + wrapColumns)));
            }
        }
        visualStarts[logicalLines] = visualLines.size();
        visualRevision = model.revision();
        visualWrapColumns = wrapColumns;
        visualLogicalLines = logicalLines;
    }

    private int currentWrapColumns() {
        int available = Math.max(1, getWidth() - getPaddingLeft() - getPaddingRight());
        return Math.max(1, (int) Math.floor(available / Math.max(1f, cellWidth)));
    }

    private int visualStartForLogicalRow(int logicalRow) {
        ensureVisualLayout();
        if (visualStarts.length == 0) return 0;
        int row = clamp(logicalRow, 0, visualStarts.length - 1);
        return visualStarts[row];
    }

    private int logicalRowAtViewportTop() {
        ensureVisualLayout();
        if (visualLines.isEmpty()) return 0;
        int visualRow = clamp((int) Math.floor(
                (getScrollY() - getPaddingTop()) / Math.max(1f, cellHeight)),
                0, visualLines.size() - 1);
        return visualLines.get(visualRow).logicalRow;
    }

    private void invalidateVisualLayout() {
        visualRevision = Long.MIN_VALUE;
        visualWrapColumns = -1;
        visualLogicalLines = -1;
    }

    private static int blend(int foreground, int background, float amount) {
        float inverse = 1f - amount;
        return Color.rgb(
                Math.round(Color.red(foreground) * amount + Color.red(background) * inverse),
                Math.round(Color.green(foreground) * amount + Color.green(background) * inverse),
                Math.round(Color.blue(foreground) * amount + Color.blue(background) * inverse));
    }

    private static int clamp(int value, int minimum, int maximum) {
        return Math.max(minimum, Math.min(maximum, value));
    }

    private static float clamp(float value, float minimum, float maximum) {
        return Math.max(minimum, Math.min(maximum, value));
    }
}
