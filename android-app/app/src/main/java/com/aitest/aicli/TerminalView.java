package com.aitest.aicli;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.util.AttributeSet;
import android.view.GestureDetector;
import android.view.MotionEvent;
import android.view.ScaleGestureDetector;
import android.view.View;
import android.widget.OverScroller;

import androidx.annotation.Nullable;

/** Native cell renderer with terminal-style scrolling and pinch zoom. */
public final class TerminalView extends View {
    private static final float MIN_TEXT_SP = 7.5f;
    private static final float MAX_TEXT_SP = 19f;

    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.SUBPIXEL_TEXT_FLAG);
    private final Paint fillPaint = new Paint();
    private final Paint cursorPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final GestureDetector gestures;
    private final ScaleGestureDetector scaleGestures;
    private final OverScroller scroller;
    private final float density;
    private final float scaledDensity;

    private VtTerminalEmulator terminal;
    private float textSizeSp = 12f;
    private float cellWidth;
    private float cellHeight;
    private float baselineOffset;
    private boolean framePosted;
    private boolean followOnFrame;
    private boolean cursorOn = true;
    private boolean attached;

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
                return true;
            }

            @Override public boolean onSingleTapUp(MotionEvent event) {
                performClick();
                return true;
            }

            @Override public boolean onScroll(MotionEvent first, MotionEvent current,
                                              float distanceX, float distanceY) {
                scrollByClamped(Math.round(distanceX), Math.round(distanceY));
                return true;
            }

            @Override public boolean onFling(MotionEvent first, MotionEvent current,
                                             float velocityX, float velocityY) {
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
        terminal = value;
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
            if (follow) scrollToClamped(getScrollX(), maximumScrollY());
            else scrollToClamped(getScrollX(), getScrollY());
            invalidate();
            awakenScrollBars();
        });
    }

    boolean isNearBottom() {
        return maximumScrollY() - getScrollY() <= Math.max(cellHeight * 2f, 32f * density);
    }

    boolean followsBottom() {
        return isNearBottom();
    }

    @Override public boolean performClick() {
        super.performClick();
        requestFocus();
        return true;
    }

    @Override public boolean onTouchEvent(MotionEvent event) {
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
        super.onDetachedFromWindow();
    }

    @Override protected void onSizeChanged(int width, int height, int oldWidth, int oldHeight) {
        super.onSizeChanged(width, height, oldWidth, oldHeight);
        scrollToClamped(getScrollX(), getScrollY());
    }

    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        canvas.drawColor(VtTerminalEmulator.DEFAULT_BACKGROUND);
        VtTerminalEmulator model = terminal;
        if (model == null) return;

        int firstRow = Math.max(0, (int) Math.floor(
                (getScrollY() - getPaddingTop()) / Math.max(1f, cellHeight)));
        int visibleRows = (int) Math.ceil(getHeight() / Math.max(1f, cellHeight)) + 2;
        int lastRow = Math.min(model.totalLines(), firstRow + visibleRows);
        int firstColumn = Math.max(0, (int) Math.floor(
                (getScrollX() - getPaddingLeft()) / Math.max(1f, cellWidth)));
        int visibleColumns = (int) Math.ceil(getWidth() / Math.max(1f, cellWidth)) + 2;

        for (int row = firstRow; row < lastRow; row++) {
            VtTerminalEmulator.Line line = model.lineAt(row);
            if (line == null) continue;
            int lastColumn = Math.min(line.columns(), firstColumn + visibleColumns);
            float top = getPaddingTop() + row * cellHeight;
            drawLine(canvas, line, firstColumn, lastColumn, top);
        }

        if (model.cursorVisible() && cursorOn) {
            int cursorRow = model.cursorDisplayRow();
            int cursorColumn = model.cursorColumn();
            if (cursorRow >= firstRow && cursorRow < lastRow &&
                    cursorColumn >= firstColumn && cursorColumn < firstColumn + visibleColumns) {
                float left = getPaddingLeft() + cursorColumn * cellWidth;
                float top = getPaddingTop() + cursorRow * cellHeight;
                canvas.drawRect(left + 0.5f, top + 0.5f,
                        left + cellWidth - 0.5f, top + cellHeight - 0.5f, cursorPaint);
            }
        }
    }

    private void drawLine(Canvas canvas, VtTerminalEmulator.Line line,
                          int firstColumn, int lastColumn, float top) {
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
                canvas.drawRect(getPaddingLeft() + column * cellWidth, top,
                        getPaddingLeft() + runEnd * cellWidth, top + cellHeight, fillPaint);
            }
            configureTextPaint(foreground, background, flags);
            for (int cell = column; cell < runEnd; cell++) {
                int codePoint = line.codePointAt(cell);
                if (codePoint <= 0 || codePoint == ' ') continue;
                String glyph = new String(Character.toChars(codePoint));
                canvas.drawText(glyph, getPaddingLeft() + cell * cellWidth,
                        top + baselineOffset, textPaint);
            }
            column = runEnd;
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
    }

    private void scrollByClamped(int deltaX, int deltaY) {
        scrollToClamped(getScrollX() + deltaX, getScrollY() + deltaY);
        awakenScrollBars();
    }

    private void scrollToClamped(int x, int y) {
        scrollTo(clamp(x, 0, maximumScrollX()), clamp(y, 0, maximumScrollY()));
    }

    private int contentWidth() {
        return terminal == null ? getWidth() : Math.round(
                getPaddingLeft() + terminal.columns() * cellWidth + getPaddingRight());
    }

    private int contentHeight() {
        return terminal == null ? getHeight() : Math.round(
                getPaddingTop() + terminal.totalLines() * cellHeight + getPaddingBottom());
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
        postInvalidateOnAnimation();
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
