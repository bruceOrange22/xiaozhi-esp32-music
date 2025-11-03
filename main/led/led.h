#ifndef _LED_H_
#define _LED_H_

class Led {
public:
    virtual ~Led() = default;
    // Set the led state based on the device state
    virtual void OnStateChanged() = 0;
    // Called when streaming (music playback) starts. Default no-op.
    virtual void OnStreamingStarted() {}
    // Called when streaming (music playback) stops. Default no-op.
    virtual void OnStreamingStopped() {}
};


class NoLed : public Led {
public:
    virtual void OnStateChanged() override {}
};

#endif // _LED_H_
