#pragma once

enum MonitorState {
    Idle,
    Monitoring,
    RGMonitoring
};

extern MonitorState state;

void FrameDataMonitor();