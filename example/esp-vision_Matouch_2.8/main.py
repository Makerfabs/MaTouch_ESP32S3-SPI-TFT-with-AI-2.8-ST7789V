import display
import espdl
import sensor
import time

MODEL = "/sdcard/espdet_pico_224_224_hand.espdl"

lcd = display.Display()

sensor.reset()
sensor.set_hmirror(False)
sensor.set_vflip(False)
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=1000)

det = espdl.ESPDet(MODEL, score=0.3, nms=0.7)

last_display = time.ticks_ms()

try:
    while True:
        img = sensor.snapshot()

        for x, y, w, h, score, category in det.detect(img):
            img.draw_rectangle(
                x, y, w, h,
                color=(255, 0, 0),
                thickness=2
            )

            img.draw_string(
                x,
                max(0, y - 12),
                "hand %.2f" % score,
                color=(255, 0, 0)
            )

        # 每 100 ms 才刷新一次 LCD，约 10 FPS
        if time.ticks_diff(time.ticks_ms(), last_display) >= 100:
            lcd.write(img)
            last_display = time.ticks_ms()

finally:
    det.deinit()
    sensor.shutdown()
    lcd.deinit()