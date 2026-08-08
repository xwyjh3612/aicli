package com.aitest.aicli;

import android.graphics.Color;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Stateful VT/ANSI screen model for the mobile companion.
 *
 * <p>The desktop remains the PTY owner. This class only interprets the same
 * output stream that Windows Terminal receives, so cursor movement, erase,
 * color and full-screen Codex/Claude repaint operations are applied in place
 * instead of being appended as duplicate chat text.</p>
 */
final class VtTerminalEmulator {
    static final int DEFAULT_FOREGROUND = Color.rgb(215, 215, 215);
    static final int DEFAULT_BACKGROUND = Color.rgb(11, 11, 11);

    static final byte STYLE_BOLD = 1;
    static final byte STYLE_DIM = 1 << 1;
    static final byte STYLE_UNDERLINE = 1 << 2;
    static final byte STYLE_INVERSE = 1 << 3;
    static final byte STYLE_HIDDEN = 1 << 4;
    static final byte STYLE_STRIKE = 1 << 5;

    private static final int CONTINUATION = -1;
    private static final int MAX_SCROLLBACK_LINES = 3000;
    private static final int SCROLLBACK_TRIM_BATCH = 128;
    private static final int STATE_GROUND = 0;
    private static final int STATE_ESCAPE = 1;
    private static final int STATE_CSI = 2;
    private static final int STATE_OSC = 3;
    private static final int STATE_OSC_ESCAPE = 4;
    private static final int STATE_STRING = 5;
    private static final int STATE_STRING_ESCAPE = 6;
    private static final int STATE_CHARSET = 7;

    private static final int[] ANSI_COLORS = {
            Color.rgb(12, 12, 12), Color.rgb(197, 15, 31),
            Color.rgb(19, 161, 14), Color.rgb(193, 156, 0),
            Color.rgb(0, 55, 218), Color.rgb(136, 23, 152),
            Color.rgb(58, 150, 221), Color.rgb(204, 204, 204),
            Color.rgb(118, 118, 118), Color.rgb(231, 72, 86),
            Color.rgb(22, 198, 12), Color.rgb(249, 241, 165),
            Color.rgb(59, 120, 255), Color.rgb(180, 0, 158),
            Color.rgb(97, 214, 214), Color.rgb(242, 242, 242)
    };

    static final class Line {
        private int[] codePoints;
        private long[] colors;
        private byte[] flags;

        Line(int columns, long fillColors, byte fillFlags) {
            codePoints = new int[Math.max(1, columns)];
            colors = new long[codePoints.length];
            flags = new byte[codePoints.length];
            clear(0, codePoints.length, fillColors, fillFlags);
        }

        int columns() { return codePoints.length; }
        int codePointAt(int column) { return codePoints[column]; }
        long colorsAt(int column) { return colors[column]; }
        byte flagsAt(int column) { return flags[column]; }

        void resize(int columns, long fillColors, byte fillFlags) {
            int nextColumns = Math.max(1, columns);
            int previous = codePoints.length;
            codePoints = Arrays.copyOf(codePoints, nextColumns);
            colors = Arrays.copyOf(colors, nextColumns);
            flags = Arrays.copyOf(flags, nextColumns);
            if (nextColumns > previous) clear(previous, nextColumns, fillColors, fillFlags);
            if (nextColumns > 0 && codePoints[nextColumns - 1] == CONTINUATION) {
                codePoints[nextColumns - 1] = ' ';
                colors[nextColumns - 1] = fillColors;
                flags[nextColumns - 1] = fillFlags;
            }
        }

        void clear(int start, int end, long fillColors, byte fillFlags) {
            int left = Math.max(0, start);
            int right = Math.min(codePoints.length, Math.max(left, end));
            Arrays.fill(codePoints, left, right, ' ');
            Arrays.fill(colors, left, right, fillColors);
            Arrays.fill(flags, left, right, fillFlags);
        }

        void put(int column, int codePoint, int width, long valueColors, byte valueFlags) {
            if (column < 0 || column >= codePoints.length) return;
            clearGlyphAt(column, valueColors, valueFlags);
            if (width == 2 && column + 1 < codePoints.length) {
                clearGlyphAt(column + 1, valueColors, valueFlags);
            }
            codePoints[column] = codePoint;
            colors[column] = valueColors;
            flags[column] = valueFlags;
            if (width == 2 && column + 1 < codePoints.length) {
                codePoints[column + 1] = CONTINUATION;
                colors[column + 1] = valueColors;
                flags[column + 1] = valueFlags;
            }
        }

        void insert(int column, int count, long fillColors, byte fillFlags) {
            if (column < 0 || column >= codePoints.length || count <= 0) return;
            int amount = Math.min(count, codePoints.length - column);
            int movable = codePoints.length - column - amount;
            if (movable > 0) {
                System.arraycopy(codePoints, column, codePoints, column + amount, movable);
                System.arraycopy(colors, column, colors, column + amount, movable);
                System.arraycopy(flags, column, flags, column + amount, movable);
            }
            clear(column, column + amount, fillColors, fillFlags);
            repairWideBoundaries(fillColors, fillFlags);
        }

        void delete(int column, int count, long fillColors, byte fillFlags) {
            if (column < 0 || column >= codePoints.length || count <= 0) return;
            int amount = Math.min(count, codePoints.length - column);
            int movable = codePoints.length - column - amount;
            if (movable > 0) {
                System.arraycopy(codePoints, column + amount, codePoints, column, movable);
                System.arraycopy(colors, column + amount, colors, column, movable);
                System.arraycopy(flags, column + amount, flags, column, movable);
            }
            clear(codePoints.length - amount, codePoints.length, fillColors, fillFlags);
            repairWideBoundaries(fillColors, fillFlags);
        }

        private void clearGlyphAt(int column, long fillColors, byte fillFlags) {
            if (column < 0 || column >= codePoints.length) return;
            if (codePoints[column] == CONTINUATION && column > 0) {
                codePoints[column - 1] = ' ';
                colors[column - 1] = fillColors;
                flags[column - 1] = fillFlags;
            }
            if (column + 1 < codePoints.length && codePoints[column + 1] == CONTINUATION) {
                codePoints[column + 1] = ' ';
                colors[column + 1] = fillColors;
                flags[column + 1] = fillFlags;
            }
            codePoints[column] = ' ';
            colors[column] = fillColors;
            flags[column] = fillFlags;
        }

        private void repairWideBoundaries(long fillColors, byte fillFlags) {
            if (codePoints.length == 0) return;
            if (codePoints[0] == CONTINUATION) {
                codePoints[0] = ' ';
                colors[0] = fillColors;
                flags[0] = fillFlags;
            }
            for (int index = 1; index < codePoints.length; index++) {
                if (codePoints[index] == CONTINUATION && codePoints[index - 1] <= 0) {
                    codePoints[index] = ' ';
                    colors[index] = fillColors;
                    flags[index] = fillFlags;
                }
            }
        }
    }

    private static final class Screen {
        final ArrayList<Line> history = new ArrayList<>();
        final ArrayList<Line> rows = new ArrayList<>();
        int cursorRow;
        int cursorColumn;
        int savedRow;
        int savedColumn;
        int scrollTop;
        int scrollBottom;

        Screen(int rowCount, int columns, long fillColors, byte fillFlags) {
            reset(rowCount, columns, fillColors, fillFlags);
        }

        void reset(int rowCount, int columns, long fillColors, byte fillFlags) {
            history.clear();
            rows.clear();
            for (int row = 0; row < rowCount; row++) {
                rows.add(new Line(columns, fillColors, fillFlags));
            }
            cursorRow = 0;
            cursorColumn = 0;
            savedRow = 0;
            savedColumn = 0;
            scrollTop = 0;
            scrollBottom = Math.max(0, rowCount - 1);
        }
    }

    private int rows;
    private int columns;
    private Screen normal;
    private Screen alternate;
    private Screen active;
    private boolean alternateActive;
    private boolean cursorVisible = true;
    private boolean autowrap = true;
    private boolean wrapPending;
    private boolean originMode;
    private boolean insertMode;
    private boolean synchronizedOutput;
    private int foreground = DEFAULT_FOREGROUND;
    private int background = DEFAULT_BACKGROUND;
    private byte styleFlags;
    private int parserState = STATE_GROUND;
    private final StringBuilder controlBuffer = new StringBuilder();
    private int lastPrintedCodePoint = ' ';

    VtTerminalEmulator(int rows, int columns) {
        reset(rows, columns);
    }

    void reset(int requestedRows, int requestedColumns) {
        rows = Math.max(5, requestedRows);
        columns = Math.max(20, requestedColumns);
        foreground = DEFAULT_FOREGROUND;
        background = DEFAULT_BACKGROUND;
        styleFlags = 0;
        normal = new Screen(rows, columns, currentColors(), styleFlags);
        alternate = new Screen(rows, columns, currentColors(), styleFlags);
        active = normal;
        alternateActive = false;
        cursorVisible = true;
        autowrap = true;
        wrapPending = false;
        originMode = false;
        insertMode = false;
        synchronizedOutput = false;
        parserState = STATE_GROUND;
        controlBuffer.setLength(0);
        lastPrintedCodePoint = ' ';
    }

    void resize(int requestedRows, int requestedColumns) {
        int nextRows = Math.max(5, requestedRows);
        int nextColumns = Math.max(20, requestedColumns);
        if (nextRows == rows && nextColumns == columns) return;
        resizeScreen(normal, nextRows, nextColumns);
        resizeScreen(alternate, nextRows, nextColumns);
        rows = nextRows;
        columns = nextColumns;
        wrapPending = false;
    }

    void feed(String data) {
        if (data == null || data.isEmpty()) return;
        for (int offset = 0; offset < data.length();) {
            int codePoint = data.codePointAt(offset);
            offset += Character.charCount(codePoint);
            process(codePoint);
        }
    }

    int rows() { return rows; }
    int columns() { return columns; }
    int totalLines() { return active.history.size() + active.rows.size(); }
    int historyLines() { return active.history.size(); }
    int cursorDisplayRow() { return active.history.size() + active.cursorRow; }
    int cursorColumn() { return active.cursorColumn; }
    boolean cursorVisible() { return cursorVisible; }
    boolean synchronizedOutput() { return synchronizedOutput; }

    Line lineAt(int displayRow) {
        if (displayRow < 0) return null;
        if (displayRow < active.history.size()) return active.history.get(displayRow);
        int row = displayRow - active.history.size();
        return row >= 0 && row < active.rows.size() ? active.rows.get(row) : null;
    }

    private void resizeScreen(Screen screen, int nextRows, int nextColumns) {
        for (Line line : screen.rows) line.resize(nextColumns, currentColors(), styleFlags);
        if (nextRows > screen.rows.size()) {
            while (screen.rows.size() < nextRows) {
                screen.rows.add(new Line(nextColumns, currentColors(), styleFlags));
            }
        } else {
            while (screen.rows.size() > nextRows) screen.rows.remove(screen.rows.size() - 1);
        }
        screen.cursorRow = clamp(screen.cursorRow, 0, nextRows - 1);
        screen.cursorColumn = clamp(screen.cursorColumn, 0, nextColumns - 1);
        screen.savedRow = clamp(screen.savedRow, 0, nextRows - 1);
        screen.savedColumn = clamp(screen.savedColumn, 0, nextColumns - 1);
        screen.scrollTop = 0;
        screen.scrollBottom = nextRows - 1;
    }

    private void process(int codePoint) {
        switch (parserState) {
            case STATE_ESCAPE:
                processEscape(codePoint);
                return;
            case STATE_CSI:
                if (codePoint >= 0x40 && codePoint <= 0x7e) {
                    dispatchCsi((char) codePoint, controlBuffer.toString());
                    controlBuffer.setLength(0);
                    parserState = STATE_GROUND;
                } else if (controlBuffer.length() < 512) {
                    controlBuffer.appendCodePoint(codePoint);
                }
                return;
            case STATE_OSC:
                if (codePoint == 0x07) parserState = STATE_GROUND;
                else if (codePoint == 0x1b) parserState = STATE_OSC_ESCAPE;
                return;
            case STATE_OSC_ESCAPE:
                parserState = codePoint == '\\' ? STATE_GROUND : STATE_OSC;
                return;
            case STATE_STRING:
                if (codePoint == 0x1b) parserState = STATE_STRING_ESCAPE;
                return;
            case STATE_STRING_ESCAPE:
                parserState = codePoint == '\\' ? STATE_GROUND : STATE_STRING;
                return;
            case STATE_CHARSET:
                parserState = STATE_GROUND;
                return;
            default:
                break;
        }

        if (codePoint == 0x1b) {
            parserState = STATE_ESCAPE;
            return;
        }
        switch (codePoint) {
            case 0x00:
            case 0x07:
            case 0x0e:
            case 0x0f:
                return;
            case 0x08:
                active.cursorColumn = Math.max(0, active.cursorColumn - 1);
                wrapPending = false;
                return;
            case 0x09:
                active.cursorColumn = Math.min(columns - 1,
                        ((active.cursorColumn / 8) + 1) * 8);
                wrapPending = false;
                return;
            case 0x0a:
            case 0x0b:
            case 0x0c:
                lineFeed();
                return;
            case 0x0d:
                active.cursorColumn = 0;
                wrapPending = false;
                return;
            case 0x7f:
                return;
            default:
                if (codePoint >= 0x20) print(codePoint);
        }
    }

    private void processEscape(int codePoint) {
        parserState = STATE_GROUND;
        switch (codePoint) {
            case '[':
                controlBuffer.setLength(0);
                parserState = STATE_CSI;
                break;
            case ']':
                parserState = STATE_OSC;
                break;
            case 'P':
            case '_':
            case '^':
            case 'X':
                parserState = STATE_STRING;
                break;
            case '(':
            case ')':
            case '*':
            case '+':
            case '#':
                parserState = STATE_CHARSET;
                break;
            case '7':
                saveCursor();
                break;
            case '8':
                restoreCursor();
                break;
            case 'D':
                lineFeed();
                break;
            case 'E':
                active.cursorColumn = 0;
                lineFeed();
                break;
            case 'M':
                reverseIndex();
                break;
            case 'c':
                reset(rows, columns);
                break;
            default:
                break;
        }
    }

    private void dispatchCsi(char command, String rawParameters) {
        char prefix = 0;
        String parameters = rawParameters;
        if (!parameters.isEmpty()) {
            char first = parameters.charAt(0);
            if (first == '?' || first == '>' || first == '!' || first == '=') {
                prefix = first;
                parameters = parameters.substring(1);
            }
        }
        int intermediate = firstIntermediate(parameters);
        String intermediateText = intermediate >= 0 ? parameters.substring(intermediate) : "";
        if (intermediate >= 0) parameters = parameters.substring(0, intermediate);
        int[] values = parseParameters(parameters);
        Screen screen = active;
        switch (command) {
            case 'A': moveCursor(-parameter(values, 0, 1), 0); break;
            case 'B': case 'e': moveCursor(parameter(values, 0, 1), 0); break;
            case 'C': case 'a': moveCursor(0, parameter(values, 0, 1)); break;
            case 'D': moveCursor(0, -parameter(values, 0, 1)); break;
            case 'E':
                screen.cursorRow = clamp(screen.cursorRow + parameter(values, 0, 1), 0, rows - 1);
                screen.cursorColumn = 0; wrapPending = false; break;
            case 'F':
                screen.cursorRow = clamp(screen.cursorRow - parameter(values, 0, 1), 0, rows - 1);
                screen.cursorColumn = 0; wrapPending = false; break;
            case 'G': case '`':
                screen.cursorColumn = clamp(parameter(values, 0, 1) - 1, 0, columns - 1);
                wrapPending = false; break;
            case 'H': case 'f':
                setCursor(parameter(values, 0, 1) - 1, parameter(values, 1, 1) - 1);
                break;
            case 'd':
                setCursor(parameter(values, 0, 1) - 1, screen.cursorColumn);
                break;
            case 'J': eraseDisplay(parameter(values, 0, 0)); break;
            case 'K': eraseLine(parameter(values, 0, 0)); break;
            case 'L': insertLines(parameter(values, 0, 1)); break;
            case 'M': deleteLines(parameter(values, 0, 1)); break;
            case '@':
                screen.rows.get(screen.cursorRow).insert(screen.cursorColumn,
                        parameter(values, 0, 1), currentColors(), styleFlags);
                break;
            case 'P':
                screen.rows.get(screen.cursorRow).delete(screen.cursorColumn,
                        parameter(values, 0, 1), currentColors(), styleFlags);
                break;
            case 'X':
                screen.rows.get(screen.cursorRow).clear(screen.cursorColumn,
                        Math.min(columns, screen.cursorColumn + parameter(values, 0, 1)),
                        currentColors(), styleFlags);
                break;
            case 'S': scrollUp(parameter(values, 0, 1)); break;
            case 'T': scrollDown(parameter(values, 0, 1)); break;
            case 'm': applySgr(values); break;
            case 'r': setScrollRegion(values); break;
            case 's': saveCursor(); break;
            case 'u': restoreCursor(); break;
            case 'b':
                int repeats = parameter(values, 0, 1);
                for (int count = 0; count < repeats; count++) print(lastPrintedCodePoint);
                break;
            case 'h': setModes(prefix, values, true); break;
            case 'l': setModes(prefix, values, false); break;
            case 'p':
                if (prefix == '!' || intermediateText.indexOf('!') >= 0) softReset();
                break;
            default:
                break;
        }
    }

    private void print(int codePoint) {
        int width = cellWidth(codePoint);
        if (width <= 0) return;
        if (wrapPending && autowrap) {
            active.cursorColumn = 0;
            lineFeed();
        }
        if (width == 2 && active.cursorColumn >= columns - 1) {
            if (autowrap) {
                active.cursorColumn = 0;
                lineFeed();
            } else {
                width = 1;
            }
        }
        if (insertMode) {
            active.rows.get(active.cursorRow).insert(active.cursorColumn, width,
                    currentColors(), styleFlags);
        }
        active.rows.get(active.cursorRow).put(active.cursorColumn, codePoint, width,
                currentColors(), styleFlags);
        lastPrintedCodePoint = codePoint;
        int next = active.cursorColumn + width;
        if (next >= columns) {
            active.cursorColumn = columns - 1;
            wrapPending = true;
        } else {
            active.cursorColumn = next;
            wrapPending = false;
        }
    }

    private void lineFeed() {
        wrapPending = false;
        if (active.cursorRow == active.scrollBottom) {
            scrollUp(1);
        } else {
            active.cursorRow = Math.min(rows - 1, active.cursorRow + 1);
        }
    }

    private void reverseIndex() {
        wrapPending = false;
        if (active.cursorRow == active.scrollTop) {
            scrollDown(1);
        } else {
            active.cursorRow = Math.max(0, active.cursorRow - 1);
        }
    }

    private void scrollUp(int requested) {
        int count = Math.min(Math.max(1, requested),
                active.scrollBottom - active.scrollTop + 1);
        for (int index = 0; index < count; index++) {
            Line removed = active.rows.remove(active.scrollTop);
            if (active.scrollTop == 0 && active.scrollBottom == rows - 1) {
                active.history.add(removed);
                trimHistory(active);
            }
            active.rows.add(active.scrollBottom,
                    new Line(columns, currentColors(), styleFlags));
        }
    }

    private void scrollDown(int requested) {
        int count = Math.min(Math.max(1, requested),
                active.scrollBottom - active.scrollTop + 1);
        for (int index = 0; index < count; index++) {
            active.rows.remove(active.scrollBottom);
            active.rows.add(active.scrollTop,
                    new Line(columns, currentColors(), styleFlags));
        }
    }

    private static void trimHistory(Screen screen) {
        if (screen.history.size() <= MAX_SCROLLBACK_LINES + SCROLLBACK_TRIM_BATCH) return;
        screen.history.subList(0, SCROLLBACK_TRIM_BATCH).clear();
    }

    private void insertLines(int requested) {
        if (active.cursorRow < active.scrollTop || active.cursorRow > active.scrollBottom) return;
        int count = Math.min(Math.max(1, requested), active.scrollBottom - active.cursorRow + 1);
        for (int index = 0; index < count; index++) {
            active.rows.remove(active.scrollBottom);
            active.rows.add(active.cursorRow,
                    new Line(columns, currentColors(), styleFlags));
        }
    }

    private void deleteLines(int requested) {
        if (active.cursorRow < active.scrollTop || active.cursorRow > active.scrollBottom) return;
        int count = Math.min(Math.max(1, requested), active.scrollBottom - active.cursorRow + 1);
        for (int index = 0; index < count; index++) {
            active.rows.remove(active.cursorRow);
            active.rows.add(active.scrollBottom,
                    new Line(columns, currentColors(), styleFlags));
        }
    }

    private void moveCursor(int rowDelta, int columnDelta) {
        int minimumRow = originMode ? active.scrollTop : 0;
        int maximumRow = originMode ? active.scrollBottom : rows - 1;
        active.cursorRow = clamp(active.cursorRow + rowDelta, minimumRow, maximumRow);
        active.cursorColumn = clamp(active.cursorColumn + columnDelta, 0, columns - 1);
        wrapPending = false;
    }

    private void setCursor(int row, int column) {
        int targetRow = originMode ? active.scrollTop + row : row;
        int minimumRow = originMode ? active.scrollTop : 0;
        int maximumRow = originMode ? active.scrollBottom : rows - 1;
        active.cursorRow = clamp(targetRow, minimumRow, maximumRow);
        active.cursorColumn = clamp(column, 0, columns - 1);
        wrapPending = false;
    }

    private void saveCursor() {
        active.savedRow = active.cursorRow;
        active.savedColumn = active.cursorColumn;
    }

    private void restoreCursor() {
        active.cursorRow = clamp(active.savedRow, 0, rows - 1);
        active.cursorColumn = clamp(active.savedColumn, 0, columns - 1);
        wrapPending = false;
    }

    private void eraseDisplay(int mode) {
        long colors = currentColors();
        if (mode == 2 || mode == 3) {
            for (Line line : active.rows) line.clear(0, columns, colors, styleFlags);
            if (mode == 3) active.history.clear();
            return;
        }
        if (mode == 1) {
            for (int row = 0; row < active.cursorRow; row++) {
                active.rows.get(row).clear(0, columns, colors, styleFlags);
            }
            active.rows.get(active.cursorRow).clear(0, active.cursorColumn + 1,
                    colors, styleFlags);
        } else {
            active.rows.get(active.cursorRow).clear(active.cursorColumn, columns,
                    colors, styleFlags);
            for (int row = active.cursorRow + 1; row < rows; row++) {
                active.rows.get(row).clear(0, columns, colors, styleFlags);
            }
        }
    }

    private void eraseLine(int mode) {
        Line line = active.rows.get(active.cursorRow);
        if (mode == 1) line.clear(0, active.cursorColumn + 1, currentColors(), styleFlags);
        else if (mode == 2) line.clear(0, columns, currentColors(), styleFlags);
        else line.clear(active.cursorColumn, columns, currentColors(), styleFlags);
    }

    private void setScrollRegion(int[] values) {
        int top = parameter(values, 0, 1) - 1;
        int bottom = parameter(values, 1, rows) - 1;
        if (top < 0 || bottom >= rows || top >= bottom) {
            top = 0;
            bottom = rows - 1;
        }
        active.scrollTop = top;
        active.scrollBottom = bottom;
        setCursor(0, 0);
    }

    private void setModes(char prefix, int[] values, boolean enabled) {
        if (values.length == 0) values = new int[]{-1};
        for (int value : values) {
            if (prefix == '?') {
                switch (value) {
                    case 6: originMode = enabled; setCursor(0, 0); break;
                    case 7: autowrap = enabled; wrapPending = false; break;
                    case 25: cursorVisible = enabled; break;
                    case 47:
                    case 1047:
                    case 1049:
                        switchAlternateScreen(enabled, value == 1049);
                        break;
                    case 2026: synchronizedOutput = enabled; break;
                    default: break;
                }
            } else if (value == 4) {
                insertMode = enabled;
            }
        }
    }

    private void switchAlternateScreen(boolean enabled, boolean saveAndRestore) {
        if (enabled == alternateActive) return;
        if (enabled) {
            if (saveAndRestore) saveCursor();
            alternate.reset(rows, columns, currentColors(), styleFlags);
            active = alternate;
            alternateActive = true;
        } else {
            active = normal;
            alternateActive = false;
            if (saveAndRestore) restoreCursor();
        }
        wrapPending = false;
    }

    private void applySgr(int[] values) {
        if (values.length == 0) values = new int[]{0};
        for (int index = 0; index < values.length; index++) {
            int value = values[index] < 0 ? 0 : values[index];
            switch (value) {
                case 0:
                    foreground = DEFAULT_FOREGROUND;
                    background = DEFAULT_BACKGROUND;
                    styleFlags = 0;
                    break;
                case 1: styleFlags |= STYLE_BOLD; break;
                case 2: styleFlags |= STYLE_DIM; break;
                case 3: break;
                case 4: styleFlags |= STYLE_UNDERLINE; break;
                case 7: styleFlags |= STYLE_INVERSE; break;
                case 8: styleFlags |= STYLE_HIDDEN; break;
                case 9: styleFlags |= STYLE_STRIKE; break;
                case 22: styleFlags &= ~(STYLE_BOLD | STYLE_DIM); break;
                case 23: break;
                case 24: styleFlags &= ~STYLE_UNDERLINE; break;
                case 27: styleFlags &= ~STYLE_INVERSE; break;
                case 28: styleFlags &= ~STYLE_HIDDEN; break;
                case 29: styleFlags &= ~STYLE_STRIKE; break;
                case 39: foreground = DEFAULT_FOREGROUND; break;
                case 49: background = DEFAULT_BACKGROUND; break;
                default:
                    if (value >= 30 && value <= 37) foreground = ANSI_COLORS[value - 30];
                    else if (value >= 40 && value <= 47) background = ANSI_COLORS[value - 40];
                    else if (value >= 90 && value <= 97) foreground = ANSI_COLORS[8 + value - 90];
                    else if (value >= 100 && value <= 107) background = ANSI_COLORS[8 + value - 100];
                    else if ((value == 38 || value == 48) && index + 1 < values.length) {
                        boolean targetForeground = value == 38;
                        int mode = values[++index];
                        if (mode == 5 && index + 1 < values.length) {
                            int color = indexedColor(values[++index]);
                            if (targetForeground) foreground = color; else background = color;
                        } else if (mode == 2 && index + 3 < values.length) {
                            int red = clamp(values[++index], 0, 255);
                            int green = clamp(values[++index], 0, 255);
                            int blue = clamp(values[++index], 0, 255);
                            int color = Color.rgb(red, green, blue);
                            if (targetForeground) foreground = color; else background = color;
                        }
                    }
                    break;
            }
        }
    }

    private void softReset() {
        cursorVisible = true;
        autowrap = true;
        originMode = false;
        insertMode = false;
        wrapPending = false;
        foreground = DEFAULT_FOREGROUND;
        background = DEFAULT_BACKGROUND;
        styleFlags = 0;
        active.scrollTop = 0;
        active.scrollBottom = rows - 1;
        setCursor(0, 0);
    }

    private long currentColors() {
        return ((long) foreground << 32) | (background & 0xffffffffL);
    }

    static int foreground(long colors) { return (int) (colors >>> 32); }
    static int background(long colors) { return (int) colors; }

    private static int indexedColor(int index) {
        int value = clamp(index, 0, 255);
        if (value < 16) return ANSI_COLORS[value];
        if (value >= 232) {
            int level = 8 + (value - 232) * 10;
            return Color.rgb(level, level, level);
        }
        int cube = value - 16;
        int red = cube / 36;
        int green = (cube / 6) % 6;
        int blue = cube % 6;
        return Color.rgb(cubeLevel(red), cubeLevel(green), cubeLevel(blue));
    }

    private static int cubeLevel(int value) { return value == 0 ? 0 : 55 + value * 40; }

    private static int[] parseParameters(String parameters) {
        if (parameters == null || parameters.isEmpty()) return new int[0];
        String normalized = parameters.replace(':', ';');
        String[] pieces = normalized.split(";", -1);
        int[] result = new int[pieces.length];
        for (int index = 0; index < pieces.length; index++) {
            if (pieces[index].isEmpty()) {
                result[index] = -1;
                continue;
            }
            try {
                result[index] = Integer.parseInt(pieces[index]);
            } catch (NumberFormatException ignored) {
                result[index] = -1;
            }
        }
        return result;
    }

    private static int firstIntermediate(String value) {
        for (int index = 0; index < value.length(); index++) {
            char character = value.charAt(index);
            if (character >= 0x20 && character <= 0x2f) return index;
        }
        return -1;
    }

    private static int parameter(int[] values, int index, int defaultValue) {
        if (index >= values.length || values[index] <= 0) return defaultValue;
        return values[index];
    }

    private static int cellWidth(int codePoint) {
        int type = Character.getType(codePoint);
        if (type == Character.NON_SPACING_MARK || type == Character.ENCLOSING_MARK ||
                type == Character.COMBINING_SPACING_MARK || codePoint == 0x200d ||
                (codePoint >= 0xfe00 && codePoint <= 0xfe0f)) return 0;
        if (codePoint >= 0x1100 && (codePoint <= 0x115f || codePoint == 0x2329 ||
                codePoint == 0x232a || (codePoint >= 0x2e80 && codePoint <= 0xa4cf &&
                codePoint != 0x303f) || (codePoint >= 0xac00 && codePoint <= 0xd7a3) ||
                (codePoint >= 0xf900 && codePoint <= 0xfaff) ||
                (codePoint >= 0xfe10 && codePoint <= 0xfe19) ||
                (codePoint >= 0xfe30 && codePoint <= 0xfe6f) ||
                (codePoint >= 0xff00 && codePoint <= 0xff60) ||
                (codePoint >= 0xffe0 && codePoint <= 0xffe6) ||
                (codePoint >= 0x1f300 && codePoint <= 0x1faff) ||
                (codePoint >= 0x20000 && codePoint <= 0x3fffd))) return 2;
        return 1;
    }

    private static int clamp(int value, int minimum, int maximum) {
        return Math.max(minimum, Math.min(maximum, value));
    }
}
