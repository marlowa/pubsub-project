package com.pubsub.fixtestclient.capture;

public record CaptureStatus(
        boolean active,
        String outputPath,
        long messageCount
) {
    public static CaptureStatus inactive() {
        return new CaptureStatus(false, null, 0);
    }
}
