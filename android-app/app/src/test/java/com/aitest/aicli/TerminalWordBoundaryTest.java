package com.aitest.aicli;

import static org.junit.Assert.assertEquals;

import org.junit.Test;

public final class TerminalWordBoundaryTest {
    @Test
    public void adjacentLatinAndChineseAreOneRegularRun() {
        int[] cells = {'a', 'b', 'c', 0x4f60, -1, 0x597d, -1, ',', 'x'};

        TerminalWordBoundary.Range range = TerminalWordBoundary.find(
                cells.length, column -> cells[column], 4);

        assertEquals(0, range.start);
        assertEquals(6, range.end);
    }

    @Test
    public void terminalDelimitersSplitPathComponents() {
        int[] cells = {'/', 't', 'm', 'p', '/', 'a', '.', 't', 'x', 't'};

        TerminalWordBoundary.Range tmp = TerminalWordBoundary.find(
                cells.length, column -> cells[column], 2);
        TerminalWordBoundary.Range extension = TerminalWordBoundary.find(
                cells.length, column -> cells[column], 8);

        assertEquals(1, tmp.start);
        assertEquals(3, tmp.end);
        assertEquals(7, extension.start);
        assertEquals(9, extension.end);
    }

    @Test
    public void adjacentDelimiterCharactersAreOneRun() {
        int[] cells = {'h', 't', 't', 'p', ':', '/', '/', 'x'};

        TerminalWordBoundary.Range range = TerminalWordBoundary.find(
                cells.length, column -> cells[column], 5);

        assertEquals(4, range.start);
        assertEquals(6, range.end);
    }

    @Test
    public void wideCharacterContinuationNormalizesToLeadingCell() {
        int[] cells = {'x', 0x4e2d, -1, 'y'};

        assertEquals(1, TerminalWordBoundary.normalizeColumn(
                cells.length, column -> cells[column], 2));
    }
}
