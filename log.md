# Sample-Transmit Tradeoff

The transmitting ESP32 will be sampling audio and transmitting audio packets in parallel. This creates a 
classic producer-consumer problem. We need to know how long we should sample audio packets before signalling
the transmitting thread to send those packets to the receiving ESP32. The sample + transmit time must be less
than 20ms (threshold subject to change).

The sampling thread will fill up a buffer at a particular rate (44.1khz), and the transmitting thread will drain the buffer at a particular rate. On average, we need the overall fill rate of the buffer to be 0. This basically means we need to transmit audio packets faster than we can sample them. The two equations we must obey then, are:

(sampling_rate * sample_time) - (transmit_rate * transmit_time) < 0
sample_time + transmit_time < 20ms

Transmit time is variable and not really controllable. It could also be different on each power-up, as it depends on channel conditions and distance between the ESP32s. On top of that, we don't really want to drop our sampling rate. In order obey equation two, then, the only lever we have at our disposal is the sampling time. This means we need to estimate the transmit time. The transmit time is variable but is probably statistically predictable. When the ESP32s connect, they can ping-pong some dummy packets. This will help us estimate 2 * transmit_time (obviously during actual operation we don't want to ping-pong packets. The server ESP32 should just be focused on receving and processing audio packets, and the client ESP32 should be focused on sampling and transmitting packets). For now we can just do this at power-up but in the future this transmit_time could be periodically re-calibrated, perhaps at a time when there isn't much audio data to transmit. 

Q: Why not just use a Network Time Protocol to synchronize the ESP32 system clocks? 
A: With the ESP32s connecting to an NTP server over WiFi, it is very hard to get them synchronized at the sub-millisecond precision needed to estimate transmit time. 

## DAC Notes
The digital to analog converter uses a DMA buffer to asynchronously load and convert data. Despite an API that allows you to load an arbitrary number of bytes into the DMA buffer, it appears that the DAC will convert *all* the data in the DMA buffer.

So for instance, if you call `dac_continuous_write_asynchronously(handle, buf_addr, data, dataLen, loadedBytes)`, the DMA will still play every byte in `buf`.

I know this because
1. The amount of time it takes to load data into the buffer and then dequeue the result corresponds precisely with the size of the buffer. If the DMA buffer is 2048 bytes long, then it takes ~46.4ms for the conversion to finish. This time corresponds to a sampling frequency of 44.1kHz --> 2048 bytes / 44.1kHz = 46.4ms. Inceasing the buffer size to 3072 bytes causes the conversion to take 69.66ms, which lines up with the 44.1kHz sample rate.
2. If I memset the DMA buffer to 0xFF, then let data be just two bytes long, containing only 0x0, I see that the DAC is still outputting a high signal. If I also zero out the DMA buffer, the DAC will output a low signal.

This  means that I need to re-architect a little bit. We basically need to completely fill the DMA buffer with data.

1. Initialize a DAC with a DMA buffer size of N bytes.
2. On the receiver (server) side, we keep receiving data from the client until we get at least N bytes. 
3. The playback task will check that there more than N bytes available. If there are, the playback task loads this onto the DMA buffer.
    - For safe margin, the playback task can wait until there are 1.5 * N bytes available. 
    - We basically want to ensure that the receiver task always has data for the playback task.

In terms of shared data structures between the two tasks:
1. Use double-buffering.
    - The receive task receives data into a back buffer, and the playback task converts data from an active buffer.
    - Both buffers are larger than the size of the DMA buffer.
    - The playback task will convert data in active buffer, then notify the receive task. The receive task will recycle any remaining data in the back buffer into the active buffer, then swap the active / background buffers and notify the playback task that new data is available. 
2. Use a single circular buffer.
    - The receive task supplies data to the circular buffer. The playback task drains data from the buffer.
    - When the receive task supplies data, it locks the buffer. When the playback task needs to drain data, it locks the buffer, copies the data it needs into a thread-local buffer (or even the DMA buffer), then frees the shared buffer. 
    - The receive task must first check the shared buffer to ensure that it has enough data, and should block / poll until there is enough data. 
    - Special care needs to be taken so that the buffer doesn't fill up. 