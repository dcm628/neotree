/*  This program is my neopixel Christmas Tree running from a Pico W 
    It has a bunch of hardcoding of things and is not meant to be a general purpose program. */

// normie files
#include <stdio.h>
#include <stdlib.h>
// RPi files
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "ws2812.pio.h"
#include "pico/multicore.h"
#include "pico/cyw43_arch.h"
// my code files
#include "dcm_rgb.hpp"
#include "neo_tree_config.hpp"
#include "dcm_physics_math.hpp"

mutex core0_data_update;


#define IS_RGBW false
#define NUM_PIXELS 1000  // matches led_1..led_1000 in dcm_rgb.hpp/.cpp; currently unused elsewhere

// default to pin 2 if the board doesn't have a default WS2812 pin defined
#define WS2812_PIN_STRING_1 2
#define WS2812_PIN_STRING_2 5
#define WS2812_PIN_STRING_3 6
#define WS2812_PIN_STRING_4 7

static inline void put_pixel(uint8_t sm, uint32_t pixel_grb) {
    pio_sm_put_blocking(pio0, sm, pixel_grb << 8u);
}

static inline uint32_t urgb_u32(uint8_t r, uint8_t g, uint8_t b) {
    return
            ((uint32_t) (r) << 8) |
            ((uint32_t) (g) << 16) |
            (uint32_t) (b);
}

void write_string(uint8_t sm_to_update)
{
    // WS2812 is timing-critical: a gap of ~50us+ between bits mid-string is
    // read by the LEDs as a reset/latch, corrupting everything downstream
    // of the gap until the next full refresh - looks exactly like a string
    // randomly flickering. At 800kHz that's 30us/pixel, so pushing one
    // string (up to 300 LEDs here) takes up to ~9ms - long enough that a
    // USB interrupt landing mid-loop (now firing far more often thanks to
    // the continuous COLOR_GROUP_RGB_UPDATE traffic from randomize_leds.py)
    // can plausibly stall the CPU long enough to trigger exactly that.
    // Masking interrupts for this window doesn't drop incoming USB data -
    // the RP2040 has a 4KB hardware DPRAM buffer the USB controller fills
    // independently of firmware/interrupt servicing (usb_dpram.h,
    // USB_DPRAM_MAX) - it just delays *processing* of what's already
    // sitting there by up to this long, which is fine against the ~150ms
    // gaps between messages this project actually sends. Scoped per-string
    // (called 4x from write_my_tree()) rather than around the whole
    // 4-string refresh, so the worst case is ~9ms instead of ~30ms and USB
    // gets a brief gap to actually drain between strings.
    uint32_t interrupts = save_and_disable_interrupts();

    // Sequential writes to the pio of all N led objects, very ugly, don't care.
    if (sm_to_update == 0)
    {
    put_pixel(sm_to_update, led_1.get_grb_word());
    put_pixel(sm_to_update, led_2.get_grb_word());
    put_pixel(sm_to_update, led_3.get_grb_word());
    put_pixel(sm_to_update, led_4.get_grb_word());
    put_pixel(sm_to_update, led_5.get_grb_word());
    put_pixel(sm_to_update, led_6.get_grb_word());
    put_pixel(sm_to_update, led_7.get_grb_word());
    put_pixel(sm_to_update, led_8.get_grb_word());
    put_pixel(sm_to_update, led_9.get_grb_word());
    put_pixel(sm_to_update, led_10.get_grb_word());
    put_pixel(sm_to_update, led_11.get_grb_word());
    put_pixel(sm_to_update, led_12.get_grb_word());
    put_pixel(sm_to_update, led_13.get_grb_word());
    put_pixel(sm_to_update, led_14.get_grb_word());
    put_pixel(sm_to_update, led_15.get_grb_word());
    put_pixel(sm_to_update, led_16.get_grb_word());
    put_pixel(sm_to_update, led_17.get_grb_word());
    put_pixel(sm_to_update, led_18.get_grb_word());
    put_pixel(sm_to_update, led_19.get_grb_word());
    put_pixel(sm_to_update, led_20.get_grb_word());
    put_pixel(sm_to_update, led_21.get_grb_word());
    put_pixel(sm_to_update, led_22.get_grb_word());
    put_pixel(sm_to_update, led_23.get_grb_word());
    put_pixel(sm_to_update, led_24.get_grb_word());
    put_pixel(sm_to_update, led_25.get_grb_word());
    put_pixel(sm_to_update, led_26.get_grb_word());
    put_pixel(sm_to_update, led_27.get_grb_word());
    put_pixel(sm_to_update, led_28.get_grb_word());
    put_pixel(sm_to_update, led_29.get_grb_word());
    put_pixel(sm_to_update, led_30.get_grb_word());
    put_pixel(sm_to_update, led_31.get_grb_word());
    put_pixel(sm_to_update, led_32.get_grb_word());
    put_pixel(sm_to_update, led_33.get_grb_word());
    put_pixel(sm_to_update, led_34.get_grb_word());
    put_pixel(sm_to_update, led_35.get_grb_word());
    put_pixel(sm_to_update, led_36.get_grb_word());
    put_pixel(sm_to_update, led_37.get_grb_word());
    put_pixel(sm_to_update, led_38.get_grb_word());
    put_pixel(sm_to_update, led_39.get_grb_word());
    put_pixel(sm_to_update, led_40.get_grb_word());
    put_pixel(sm_to_update, led_41.get_grb_word());
    put_pixel(sm_to_update, led_42.get_grb_word());
    put_pixel(sm_to_update, led_43.get_grb_word());
    put_pixel(sm_to_update, led_44.get_grb_word());
    put_pixel(sm_to_update, led_45.get_grb_word());
    put_pixel(sm_to_update, led_46.get_grb_word());
    put_pixel(sm_to_update, led_47.get_grb_word());
    put_pixel(sm_to_update, led_48.get_grb_word());
    put_pixel(sm_to_update, led_49.get_grb_word());
    put_pixel(sm_to_update, led_50.get_grb_word());
    put_pixel(sm_to_update, led_51.get_grb_word());
    put_pixel(sm_to_update, led_52.get_grb_word());
    put_pixel(sm_to_update, led_53.get_grb_word());
    put_pixel(sm_to_update, led_54.get_grb_word());
    put_pixel(sm_to_update, led_55.get_grb_word());
    put_pixel(sm_to_update, led_56.get_grb_word());
    put_pixel(sm_to_update, led_57.get_grb_word());
    put_pixel(sm_to_update, led_58.get_grb_word());
    put_pixel(sm_to_update, led_59.get_grb_word());
    put_pixel(sm_to_update, led_60.get_grb_word());
    put_pixel(sm_to_update, led_61.get_grb_word());
    put_pixel(sm_to_update, led_62.get_grb_word());
    put_pixel(sm_to_update, led_63.get_grb_word());
    put_pixel(sm_to_update, led_64.get_grb_word());
    put_pixel(sm_to_update, led_65.get_grb_word());
    put_pixel(sm_to_update, led_66.get_grb_word());
    put_pixel(sm_to_update, led_67.get_grb_word());
    put_pixel(sm_to_update, led_68.get_grb_word());
    put_pixel(sm_to_update, led_69.get_grb_word());
    put_pixel(sm_to_update, led_70.get_grb_word());
    put_pixel(sm_to_update, led_71.get_grb_word());
    put_pixel(sm_to_update, led_72.get_grb_word());
    put_pixel(sm_to_update, led_73.get_grb_word());
    put_pixel(sm_to_update, led_74.get_grb_word());
    put_pixel(sm_to_update, led_75.get_grb_word());
    put_pixel(sm_to_update, led_76.get_grb_word());
    put_pixel(sm_to_update, led_77.get_grb_word());
    put_pixel(sm_to_update, led_78.get_grb_word());
    put_pixel(sm_to_update, led_79.get_grb_word());
    put_pixel(sm_to_update, led_80.get_grb_word());
    put_pixel(sm_to_update, led_81.get_grb_word());
    put_pixel(sm_to_update, led_82.get_grb_word());
    put_pixel(sm_to_update, led_83.get_grb_word());
    put_pixel(sm_to_update, led_84.get_grb_word());
    put_pixel(sm_to_update, led_85.get_grb_word());
    put_pixel(sm_to_update, led_86.get_grb_word());
    put_pixel(sm_to_update, led_87.get_grb_word());
    put_pixel(sm_to_update, led_88.get_grb_word());
    put_pixel(sm_to_update, led_89.get_grb_word());
    put_pixel(sm_to_update, led_90.get_grb_word());
    put_pixel(sm_to_update, led_91.get_grb_word());
    put_pixel(sm_to_update, led_92.get_grb_word());
    put_pixel(sm_to_update, led_93.get_grb_word());
    put_pixel(sm_to_update, led_94.get_grb_word());
    put_pixel(sm_to_update, led_95.get_grb_word());
    put_pixel(sm_to_update, led_96.get_grb_word());
    put_pixel(sm_to_update, led_97.get_grb_word());
    put_pixel(sm_to_update, led_98.get_grb_word());
    put_pixel(sm_to_update, led_99.get_grb_word());
    put_pixel(sm_to_update, led_100.get_grb_word());
    put_pixel(sm_to_update, led_101.get_grb_word());
    put_pixel(sm_to_update, led_102.get_grb_word());
    put_pixel(sm_to_update, led_103.get_grb_word());
    put_pixel(sm_to_update, led_104.get_grb_word());
    put_pixel(sm_to_update, led_105.get_grb_word());
    put_pixel(sm_to_update, led_106.get_grb_word());
    put_pixel(sm_to_update, led_107.get_grb_word());
    put_pixel(sm_to_update, led_108.get_grb_word());
    put_pixel(sm_to_update, led_109.get_grb_word());
    put_pixel(sm_to_update, led_110.get_grb_word());
    put_pixel(sm_to_update, led_111.get_grb_word());
    put_pixel(sm_to_update, led_112.get_grb_word());
    put_pixel(sm_to_update, led_113.get_grb_word());
    put_pixel(sm_to_update, led_114.get_grb_word());
    put_pixel(sm_to_update, led_115.get_grb_word());
    put_pixel(sm_to_update, led_116.get_grb_word());
    put_pixel(sm_to_update, led_117.get_grb_word());
    put_pixel(sm_to_update, led_118.get_grb_word());
    put_pixel(sm_to_update, led_119.get_grb_word());
    put_pixel(sm_to_update, led_120.get_grb_word());
    put_pixel(sm_to_update, led_121.get_grb_word());
    put_pixel(sm_to_update, led_122.get_grb_word());
    put_pixel(sm_to_update, led_123.get_grb_word());
    put_pixel(sm_to_update, led_124.get_grb_word());
    put_pixel(sm_to_update, led_125.get_grb_word());
    put_pixel(sm_to_update, led_126.get_grb_word());
    put_pixel(sm_to_update, led_127.get_grb_word());
    put_pixel(sm_to_update, led_128.get_grb_word());
    put_pixel(sm_to_update, led_129.get_grb_word());
    put_pixel(sm_to_update, led_130.get_grb_word());
    put_pixel(sm_to_update, led_131.get_grb_word());
    put_pixel(sm_to_update, led_132.get_grb_word());
    put_pixel(sm_to_update, led_133.get_grb_word());
    put_pixel(sm_to_update, led_134.get_grb_word());
    put_pixel(sm_to_update, led_135.get_grb_word());
    put_pixel(sm_to_update, led_136.get_grb_word());
    put_pixel(sm_to_update, led_137.get_grb_word());
    put_pixel(sm_to_update, led_138.get_grb_word());
    put_pixel(sm_to_update, led_139.get_grb_word());
    put_pixel(sm_to_update, led_140.get_grb_word());
    put_pixel(sm_to_update, led_141.get_grb_word());
    put_pixel(sm_to_update, led_142.get_grb_word());
    put_pixel(sm_to_update, led_143.get_grb_word());
    put_pixel(sm_to_update, led_144.get_grb_word());
    put_pixel(sm_to_update, led_145.get_grb_word());
    put_pixel(sm_to_update, led_146.get_grb_word());
    put_pixel(sm_to_update, led_147.get_grb_word());
    put_pixel(sm_to_update, led_148.get_grb_word());
    put_pixel(sm_to_update, led_149.get_grb_word());
    put_pixel(sm_to_update, led_150.get_grb_word());
    put_pixel(sm_to_update, led_151.get_grb_word());
    put_pixel(sm_to_update, led_152.get_grb_word());
    put_pixel(sm_to_update, led_153.get_grb_word());
    put_pixel(sm_to_update, led_154.get_grb_word());
    put_pixel(sm_to_update, led_155.get_grb_word());
    put_pixel(sm_to_update, led_156.get_grb_word());
    put_pixel(sm_to_update, led_157.get_grb_word());
    put_pixel(sm_to_update, led_158.get_grb_word());
    put_pixel(sm_to_update, led_159.get_grb_word());
    put_pixel(sm_to_update, led_160.get_grb_word());
    put_pixel(sm_to_update, led_161.get_grb_word());
    put_pixel(sm_to_update, led_162.get_grb_word());
    put_pixel(sm_to_update, led_163.get_grb_word());
    put_pixel(sm_to_update, led_164.get_grb_word());
    put_pixel(sm_to_update, led_165.get_grb_word());
    put_pixel(sm_to_update, led_166.get_grb_word());
    put_pixel(sm_to_update, led_167.get_grb_word());
    put_pixel(sm_to_update, led_168.get_grb_word());
    put_pixel(sm_to_update, led_169.get_grb_word());
    put_pixel(sm_to_update, led_170.get_grb_word());
    put_pixel(sm_to_update, led_171.get_grb_word());
    put_pixel(sm_to_update, led_172.get_grb_word());
    put_pixel(sm_to_update, led_173.get_grb_word());
    put_pixel(sm_to_update, led_174.get_grb_word());
    put_pixel(sm_to_update, led_175.get_grb_word());
    put_pixel(sm_to_update, led_176.get_grb_word());
    put_pixel(sm_to_update, led_177.get_grb_word());
    put_pixel(sm_to_update, led_178.get_grb_word());
    put_pixel(sm_to_update, led_179.get_grb_word());
    put_pixel(sm_to_update, led_180.get_grb_word());
    put_pixel(sm_to_update, led_181.get_grb_word());
    put_pixel(sm_to_update, led_182.get_grb_word());
    put_pixel(sm_to_update, led_183.get_grb_word());
    put_pixel(sm_to_update, led_184.get_grb_word());
    put_pixel(sm_to_update, led_185.get_grb_word());
    put_pixel(sm_to_update, led_186.get_grb_word());
    put_pixel(sm_to_update, led_187.get_grb_word());
    put_pixel(sm_to_update, led_188.get_grb_word());
    put_pixel(sm_to_update, led_189.get_grb_word());
    put_pixel(sm_to_update, led_190.get_grb_word());
    put_pixel(sm_to_update, led_191.get_grb_word());
    put_pixel(sm_to_update, led_192.get_grb_word());
    put_pixel(sm_to_update, led_193.get_grb_word());
    put_pixel(sm_to_update, led_194.get_grb_word());
    put_pixel(sm_to_update, led_195.get_grb_word());
    put_pixel(sm_to_update, led_196.get_grb_word());
    put_pixel(sm_to_update, led_197.get_grb_word());
    put_pixel(sm_to_update, led_198.get_grb_word());
    put_pixel(sm_to_update, led_199.get_grb_word());
    put_pixel(sm_to_update, led_200.get_grb_word());
    put_pixel(sm_to_update, led_201.get_grb_word());
    put_pixel(sm_to_update, led_202.get_grb_word());
    put_pixel(sm_to_update, led_203.get_grb_word());
    put_pixel(sm_to_update, led_204.get_grb_word());
    put_pixel(sm_to_update, led_205.get_grb_word());
    put_pixel(sm_to_update, led_206.get_grb_word());
    put_pixel(sm_to_update, led_207.get_grb_word());
    put_pixel(sm_to_update, led_208.get_grb_word());
    put_pixel(sm_to_update, led_209.get_grb_word());
    put_pixel(sm_to_update, led_210.get_grb_word());
    put_pixel(sm_to_update, led_211.get_grb_word());
    put_pixel(sm_to_update, led_212.get_grb_word());
    put_pixel(sm_to_update, led_213.get_grb_word());
    put_pixel(sm_to_update, led_214.get_grb_word());
    put_pixel(sm_to_update, led_215.get_grb_word());
    put_pixel(sm_to_update, led_216.get_grb_word());
    put_pixel(sm_to_update, led_217.get_grb_word());
    put_pixel(sm_to_update, led_218.get_grb_word());
    put_pixel(sm_to_update, led_219.get_grb_word());
    put_pixel(sm_to_update, led_220.get_grb_word());
    put_pixel(sm_to_update, led_221.get_grb_word());
    put_pixel(sm_to_update, led_222.get_grb_word());
    put_pixel(sm_to_update, led_223.get_grb_word());
    put_pixel(sm_to_update, led_224.get_grb_word());
    put_pixel(sm_to_update, led_225.get_grb_word());
    put_pixel(sm_to_update, led_226.get_grb_word());
    put_pixel(sm_to_update, led_227.get_grb_word());
    put_pixel(sm_to_update, led_228.get_grb_word());
    put_pixel(sm_to_update, led_229.get_grb_word());
    put_pixel(sm_to_update, led_230.get_grb_word());
    put_pixel(sm_to_update, led_231.get_grb_word());
    put_pixel(sm_to_update, led_232.get_grb_word());
    put_pixel(sm_to_update, led_233.get_grb_word());
    put_pixel(sm_to_update, led_234.get_grb_word());
    put_pixel(sm_to_update, led_235.get_grb_word());
    put_pixel(sm_to_update, led_236.get_grb_word());
    put_pixel(sm_to_update, led_237.get_grb_word());
    put_pixel(sm_to_update, led_238.get_grb_word());
    put_pixel(sm_to_update, led_239.get_grb_word());
    put_pixel(sm_to_update, led_240.get_grb_word());
    put_pixel(sm_to_update, led_241.get_grb_word());
    put_pixel(sm_to_update, led_242.get_grb_word());
    put_pixel(sm_to_update, led_243.get_grb_word());
    put_pixel(sm_to_update, led_244.get_grb_word());
    put_pixel(sm_to_update, led_245.get_grb_word());
    put_pixel(sm_to_update, led_246.get_grb_word());
    put_pixel(sm_to_update, led_247.get_grb_word());
    put_pixel(sm_to_update, led_248.get_grb_word());
    put_pixel(sm_to_update, led_249.get_grb_word());
    put_pixel(sm_to_update, led_250.get_grb_word());
    put_pixel(sm_to_update, led_251.get_grb_word());
    put_pixel(sm_to_update, led_252.get_grb_word());
    put_pixel(sm_to_update, led_253.get_grb_word());
    put_pixel(sm_to_update, led_254.get_grb_word());
    put_pixel(sm_to_update, led_255.get_grb_word());
    put_pixel(sm_to_update, led_256.get_grb_word());
    put_pixel(sm_to_update, led_257.get_grb_word());
    put_pixel(sm_to_update, led_258.get_grb_word());
    put_pixel(sm_to_update, led_259.get_grb_word());
    put_pixel(sm_to_update, led_260.get_grb_word());
    put_pixel(sm_to_update, led_261.get_grb_word());
    put_pixel(sm_to_update, led_262.get_grb_word());
    put_pixel(sm_to_update, led_263.get_grb_word());
    put_pixel(sm_to_update, led_264.get_grb_word());
    put_pixel(sm_to_update, led_265.get_grb_word());
    put_pixel(sm_to_update, led_266.get_grb_word());
    put_pixel(sm_to_update, led_267.get_grb_word());
    put_pixel(sm_to_update, led_268.get_grb_word());
    put_pixel(sm_to_update, led_269.get_grb_word());
    put_pixel(sm_to_update, led_270.get_grb_word());
    put_pixel(sm_to_update, led_271.get_grb_word());
    put_pixel(sm_to_update, led_272.get_grb_word());
    put_pixel(sm_to_update, led_273.get_grb_word());
    put_pixel(sm_to_update, led_274.get_grb_word());
    put_pixel(sm_to_update, led_275.get_grb_word());
    put_pixel(sm_to_update, led_276.get_grb_word());
    put_pixel(sm_to_update, led_277.get_grb_word());
    put_pixel(sm_to_update, led_278.get_grb_word());
    put_pixel(sm_to_update, led_279.get_grb_word());
    put_pixel(sm_to_update, led_280.get_grb_word());
    put_pixel(sm_to_update, led_281.get_grb_word());
    put_pixel(sm_to_update, led_282.get_grb_word());
    put_pixel(sm_to_update, led_283.get_grb_word());
    put_pixel(sm_to_update, led_284.get_grb_word());
    put_pixel(sm_to_update, led_285.get_grb_word());
    put_pixel(sm_to_update, led_286.get_grb_word());
    put_pixel(sm_to_update, led_287.get_grb_word());
    put_pixel(sm_to_update, led_288.get_grb_word());
    put_pixel(sm_to_update, led_289.get_grb_word());
    put_pixel(sm_to_update, led_290.get_grb_word());
    put_pixel(sm_to_update, led_291.get_grb_word());
    put_pixel(sm_to_update, led_292.get_grb_word());
    put_pixel(sm_to_update, led_293.get_grb_word());
    put_pixel(sm_to_update, led_294.get_grb_word());
    put_pixel(sm_to_update, led_295.get_grb_word());
    put_pixel(sm_to_update, led_296.get_grb_word());
    put_pixel(sm_to_update, led_297.get_grb_word());
    put_pixel(sm_to_update, led_298.get_grb_word());
    put_pixel(sm_to_update, led_299.get_grb_word());
    put_pixel(sm_to_update, led_300.get_grb_word());
    }
    else if (sm_to_update == 1)
    {
    put_pixel(sm_to_update, led_301.get_grb_word());
    put_pixel(sm_to_update, led_302.get_grb_word());
    put_pixel(sm_to_update, led_303.get_grb_word());
    put_pixel(sm_to_update, led_304.get_grb_word());
    put_pixel(sm_to_update, led_305.get_grb_word());
    put_pixel(sm_to_update, led_306.get_grb_word());
    put_pixel(sm_to_update, led_307.get_grb_word());
    put_pixel(sm_to_update, led_308.get_grb_word());
    put_pixel(sm_to_update, led_309.get_grb_word());
    put_pixel(sm_to_update, led_310.get_grb_word());
    put_pixel(sm_to_update, led_311.get_grb_word());
    put_pixel(sm_to_update, led_312.get_grb_word());
    put_pixel(sm_to_update, led_313.get_grb_word());
    put_pixel(sm_to_update, led_314.get_grb_word());
    put_pixel(sm_to_update, led_315.get_grb_word());
    put_pixel(sm_to_update, led_316.get_grb_word());
    put_pixel(sm_to_update, led_317.get_grb_word());
    put_pixel(sm_to_update, led_318.get_grb_word());
    put_pixel(sm_to_update, led_319.get_grb_word());
    put_pixel(sm_to_update, led_320.get_grb_word());
    put_pixel(sm_to_update, led_321.get_grb_word());
    put_pixel(sm_to_update, led_322.get_grb_word());
    put_pixel(sm_to_update, led_323.get_grb_word());
    put_pixel(sm_to_update, led_324.get_grb_word());
    put_pixel(sm_to_update, led_325.get_grb_word());
    put_pixel(sm_to_update, led_326.get_grb_word());
    put_pixel(sm_to_update, led_327.get_grb_word());
    put_pixel(sm_to_update, led_328.get_grb_word());
    put_pixel(sm_to_update, led_329.get_grb_word());
    put_pixel(sm_to_update, led_330.get_grb_word());
    put_pixel(sm_to_update, led_331.get_grb_word());
    put_pixel(sm_to_update, led_332.get_grb_word());
    put_pixel(sm_to_update, led_333.get_grb_word());
    put_pixel(sm_to_update, led_334.get_grb_word());
    put_pixel(sm_to_update, led_335.get_grb_word());
    put_pixel(sm_to_update, led_336.get_grb_word());
    put_pixel(sm_to_update, led_337.get_grb_word());
    put_pixel(sm_to_update, led_338.get_grb_word());
    put_pixel(sm_to_update, led_339.get_grb_word());
    put_pixel(sm_to_update, led_340.get_grb_word());
    put_pixel(sm_to_update, led_341.get_grb_word());
    put_pixel(sm_to_update, led_342.get_grb_word());
    put_pixel(sm_to_update, led_343.get_grb_word());
    put_pixel(sm_to_update, led_344.get_grb_word());
    put_pixel(sm_to_update, led_345.get_grb_word());
    put_pixel(sm_to_update, led_346.get_grb_word());
    put_pixel(sm_to_update, led_347.get_grb_word());
    put_pixel(sm_to_update, led_348.get_grb_word());
    put_pixel(sm_to_update, led_349.get_grb_word());
    put_pixel(sm_to_update, led_350.get_grb_word());
    put_pixel(sm_to_update, led_351.get_grb_word());
    put_pixel(sm_to_update, led_352.get_grb_word());
    put_pixel(sm_to_update, led_353.get_grb_word());
    put_pixel(sm_to_update, led_354.get_grb_word());
    put_pixel(sm_to_update, led_355.get_grb_word());
    put_pixel(sm_to_update, led_356.get_grb_word());
    put_pixel(sm_to_update, led_357.get_grb_word());
    put_pixel(sm_to_update, led_358.get_grb_word());
    put_pixel(sm_to_update, led_359.get_grb_word());
    put_pixel(sm_to_update, led_360.get_grb_word());
    put_pixel(sm_to_update, led_361.get_grb_word());
    put_pixel(sm_to_update, led_362.get_grb_word());
    put_pixel(sm_to_update, led_363.get_grb_word());
    put_pixel(sm_to_update, led_364.get_grb_word());
    put_pixel(sm_to_update, led_365.get_grb_word());
    put_pixel(sm_to_update, led_366.get_grb_word());
    put_pixel(sm_to_update, led_367.get_grb_word());
    put_pixel(sm_to_update, led_368.get_grb_word());
    put_pixel(sm_to_update, led_369.get_grb_word());
    put_pixel(sm_to_update, led_370.get_grb_word());
    put_pixel(sm_to_update, led_371.get_grb_word());
    put_pixel(sm_to_update, led_372.get_grb_word());
    put_pixel(sm_to_update, led_373.get_grb_word());
    put_pixel(sm_to_update, led_374.get_grb_word());
    put_pixel(sm_to_update, led_375.get_grb_word());
    put_pixel(sm_to_update, led_376.get_grb_word());
    put_pixel(sm_to_update, led_377.get_grb_word());
    put_pixel(sm_to_update, led_378.get_grb_word());
    put_pixel(sm_to_update, led_379.get_grb_word());
    put_pixel(sm_to_update, led_380.get_grb_word());
    put_pixel(sm_to_update, led_381.get_grb_word());
    put_pixel(sm_to_update, led_382.get_grb_word());
    put_pixel(sm_to_update, led_383.get_grb_word());
    put_pixel(sm_to_update, led_384.get_grb_word());
    put_pixel(sm_to_update, led_385.get_grb_word());
    put_pixel(sm_to_update, led_386.get_grb_word());
    put_pixel(sm_to_update, led_387.get_grb_word());
    put_pixel(sm_to_update, led_388.get_grb_word());
    put_pixel(sm_to_update, led_389.get_grb_word());
    put_pixel(sm_to_update, led_390.get_grb_word());
    put_pixel(sm_to_update, led_391.get_grb_word());
    put_pixel(sm_to_update, led_392.get_grb_word());
    put_pixel(sm_to_update, led_393.get_grb_word());
    put_pixel(sm_to_update, led_394.get_grb_word());
    put_pixel(sm_to_update, led_395.get_grb_word());
    put_pixel(sm_to_update, led_396.get_grb_word());
    put_pixel(sm_to_update, led_397.get_grb_word());
    put_pixel(sm_to_update, led_398.get_grb_word());
    put_pixel(sm_to_update, led_399.get_grb_word());
    put_pixel(sm_to_update, led_400.get_grb_word());
    put_pixel(sm_to_update, led_401.get_grb_word());
    put_pixel(sm_to_update, led_402.get_grb_word());
    put_pixel(sm_to_update, led_403.get_grb_word());
    put_pixel(sm_to_update, led_404.get_grb_word());
    put_pixel(sm_to_update, led_405.get_grb_word());
    put_pixel(sm_to_update, led_406.get_grb_word());
    put_pixel(sm_to_update, led_407.get_grb_word());
    put_pixel(sm_to_update, led_408.get_grb_word());
    put_pixel(sm_to_update, led_409.get_grb_word());
    put_pixel(sm_to_update, led_410.get_grb_word());
    put_pixel(sm_to_update, led_411.get_grb_word());
    put_pixel(sm_to_update, led_412.get_grb_word());
    put_pixel(sm_to_update, led_413.get_grb_word());
    put_pixel(sm_to_update, led_414.get_grb_word());
    put_pixel(sm_to_update, led_415.get_grb_word());
    put_pixel(sm_to_update, led_416.get_grb_word());
    put_pixel(sm_to_update, led_417.get_grb_word());
    put_pixel(sm_to_update, led_418.get_grb_word());
    put_pixel(sm_to_update, led_419.get_grb_word());
    put_pixel(sm_to_update, led_420.get_grb_word());
    put_pixel(sm_to_update, led_421.get_grb_word());
    put_pixel(sm_to_update, led_422.get_grb_word());
    put_pixel(sm_to_update, led_423.get_grb_word());
    put_pixel(sm_to_update, led_424.get_grb_word());
    put_pixel(sm_to_update, led_425.get_grb_word());
    put_pixel(sm_to_update, led_426.get_grb_word());
    put_pixel(sm_to_update, led_427.get_grb_word());
    put_pixel(sm_to_update, led_428.get_grb_word());
    put_pixel(sm_to_update, led_429.get_grb_word());
    put_pixel(sm_to_update, led_430.get_grb_word());
    put_pixel(sm_to_update, led_431.get_grb_word());
    put_pixel(sm_to_update, led_432.get_grb_word());
    put_pixel(sm_to_update, led_433.get_grb_word());
    put_pixel(sm_to_update, led_434.get_grb_word());
    put_pixel(sm_to_update, led_435.get_grb_word());
    put_pixel(sm_to_update, led_436.get_grb_word());
    put_pixel(sm_to_update, led_437.get_grb_word());
    put_pixel(sm_to_update, led_438.get_grb_word());
    put_pixel(sm_to_update, led_439.get_grb_word());
    put_pixel(sm_to_update, led_440.get_grb_word());
    put_pixel(sm_to_update, led_441.get_grb_word());
    put_pixel(sm_to_update, led_442.get_grb_word());
    put_pixel(sm_to_update, led_443.get_grb_word());
    put_pixel(sm_to_update, led_444.get_grb_word());
    put_pixel(sm_to_update, led_445.get_grb_word());
    put_pixel(sm_to_update, led_446.get_grb_word());
    put_pixel(sm_to_update, led_447.get_grb_word());
    put_pixel(sm_to_update, led_448.get_grb_word());
    put_pixel(sm_to_update, led_449.get_grb_word());
    put_pixel(sm_to_update, led_450.get_grb_word());
    put_pixel(sm_to_update, led_451.get_grb_word());
    put_pixel(sm_to_update, led_452.get_grb_word());
    put_pixel(sm_to_update, led_453.get_grb_word());
    put_pixel(sm_to_update, led_454.get_grb_word());
    put_pixel(sm_to_update, led_455.get_grb_word());
    put_pixel(sm_to_update, led_456.get_grb_word());
    put_pixel(sm_to_update, led_457.get_grb_word());
    put_pixel(sm_to_update, led_458.get_grb_word());
    put_pixel(sm_to_update, led_459.get_grb_word());
    put_pixel(sm_to_update, led_460.get_grb_word());
    put_pixel(sm_to_update, led_461.get_grb_word());
    put_pixel(sm_to_update, led_462.get_grb_word());
    put_pixel(sm_to_update, led_463.get_grb_word());
    put_pixel(sm_to_update, led_464.get_grb_word());
    put_pixel(sm_to_update, led_465.get_grb_word());
    put_pixel(sm_to_update, led_466.get_grb_word());
    put_pixel(sm_to_update, led_467.get_grb_word());
    put_pixel(sm_to_update, led_468.get_grb_word());
    put_pixel(sm_to_update, led_469.get_grb_word());
    put_pixel(sm_to_update, led_470.get_grb_word());
    put_pixel(sm_to_update, led_471.get_grb_word());
    put_pixel(sm_to_update, led_472.get_grb_word());
    put_pixel(sm_to_update, led_473.get_grb_word());
    put_pixel(sm_to_update, led_474.get_grb_word());
    put_pixel(sm_to_update, led_475.get_grb_word());
    put_pixel(sm_to_update, led_476.get_grb_word());
    put_pixel(sm_to_update, led_477.get_grb_word());
    put_pixel(sm_to_update, led_478.get_grb_word());
    put_pixel(sm_to_update, led_479.get_grb_word());
    put_pixel(sm_to_update, led_480.get_grb_word());
    put_pixel(sm_to_update, led_481.get_grb_word());
    put_pixel(sm_to_update, led_482.get_grb_word());
    put_pixel(sm_to_update, led_483.get_grb_word());
    put_pixel(sm_to_update, led_484.get_grb_word());
    put_pixel(sm_to_update, led_485.get_grb_word());
    put_pixel(sm_to_update, led_486.get_grb_word());
    put_pixel(sm_to_update, led_487.get_grb_word());
    put_pixel(sm_to_update, led_488.get_grb_word());
    put_pixel(sm_to_update, led_489.get_grb_word());
    put_pixel(sm_to_update, led_490.get_grb_word());
    put_pixel(sm_to_update, led_491.get_grb_word());
    put_pixel(sm_to_update, led_492.get_grb_word());
    put_pixel(sm_to_update, led_493.get_grb_word());
    put_pixel(sm_to_update, led_494.get_grb_word());
    put_pixel(sm_to_update, led_495.get_grb_word());
    put_pixel(sm_to_update, led_496.get_grb_word());
    put_pixel(sm_to_update, led_497.get_grb_word());
    put_pixel(sm_to_update, led_498.get_grb_word());
    put_pixel(sm_to_update, led_499.get_grb_word());
    put_pixel(sm_to_update, led_500.get_grb_word());
    put_pixel(sm_to_update, led_501.get_grb_word());
    put_pixel(sm_to_update, led_502.get_grb_word());
    put_pixel(sm_to_update, led_503.get_grb_word());
    put_pixel(sm_to_update, led_504.get_grb_word());
    put_pixel(sm_to_update, led_505.get_grb_word());
    put_pixel(sm_to_update, led_506.get_grb_word());
    put_pixel(sm_to_update, led_507.get_grb_word());
    put_pixel(sm_to_update, led_508.get_grb_word());
    put_pixel(sm_to_update, led_509.get_grb_word());
    put_pixel(sm_to_update, led_510.get_grb_word());
    put_pixel(sm_to_update, led_511.get_grb_word());
    put_pixel(sm_to_update, led_512.get_grb_word());
    put_pixel(sm_to_update, led_513.get_grb_word());
    put_pixel(sm_to_update, led_514.get_grb_word());
    put_pixel(sm_to_update, led_515.get_grb_word());
    put_pixel(sm_to_update, led_516.get_grb_word());
    put_pixel(sm_to_update, led_517.get_grb_word());
    put_pixel(sm_to_update, led_518.get_grb_word());
    put_pixel(sm_to_update, led_519.get_grb_word());
    put_pixel(sm_to_update, led_520.get_grb_word());
    put_pixel(sm_to_update, led_521.get_grb_word());
    put_pixel(sm_to_update, led_522.get_grb_word());
    put_pixel(sm_to_update, led_523.get_grb_word());
    put_pixel(sm_to_update, led_524.get_grb_word());
    put_pixel(sm_to_update, led_525.get_grb_word());
    put_pixel(sm_to_update, led_526.get_grb_word());
    put_pixel(sm_to_update, led_527.get_grb_word());
    put_pixel(sm_to_update, led_528.get_grb_word());
    put_pixel(sm_to_update, led_529.get_grb_word());
    put_pixel(sm_to_update, led_530.get_grb_word());
    put_pixel(sm_to_update, led_531.get_grb_word());
    put_pixel(sm_to_update, led_532.get_grb_word());
    put_pixel(sm_to_update, led_533.get_grb_word());
    put_pixel(sm_to_update, led_534.get_grb_word());
    put_pixel(sm_to_update, led_535.get_grb_word());
    put_pixel(sm_to_update, led_536.get_grb_word());
    put_pixel(sm_to_update, led_537.get_grb_word());
    put_pixel(sm_to_update, led_538.get_grb_word());
    put_pixel(sm_to_update, led_539.get_grb_word());
    put_pixel(sm_to_update, led_540.get_grb_word());
    put_pixel(sm_to_update, led_541.get_grb_word());
    put_pixel(sm_to_update, led_542.get_grb_word());
    put_pixel(sm_to_update, led_543.get_grb_word());
    put_pixel(sm_to_update, led_544.get_grb_word());
    put_pixel(sm_to_update, led_545.get_grb_word());
    put_pixel(sm_to_update, led_546.get_grb_word());
    put_pixel(sm_to_update, led_547.get_grb_word());
    put_pixel(sm_to_update, led_548.get_grb_word());
    put_pixel(sm_to_update, led_549.get_grb_word());
    put_pixel(sm_to_update, led_550.get_grb_word());
    put_pixel(sm_to_update, led_551.get_grb_word());
    put_pixel(sm_to_update, led_552.get_grb_word());
    put_pixel(sm_to_update, led_553.get_grb_word());
    put_pixel(sm_to_update, led_554.get_grb_word());
    put_pixel(sm_to_update, led_555.get_grb_word());
    put_pixel(sm_to_update, led_556.get_grb_word());
    put_pixel(sm_to_update, led_557.get_grb_word());
    put_pixel(sm_to_update, led_558.get_grb_word());
    put_pixel(sm_to_update, led_559.get_grb_word());
    put_pixel(sm_to_update, led_560.get_grb_word());
    put_pixel(sm_to_update, led_561.get_grb_word());
    put_pixel(sm_to_update, led_562.get_grb_word());
    put_pixel(sm_to_update, led_563.get_grb_word());
    put_pixel(sm_to_update, led_564.get_grb_word());
    put_pixel(sm_to_update, led_565.get_grb_word());
    put_pixel(sm_to_update, led_566.get_grb_word());
    put_pixel(sm_to_update, led_567.get_grb_word());
    put_pixel(sm_to_update, led_568.get_grb_word());
    put_pixel(sm_to_update, led_569.get_grb_word());
    put_pixel(sm_to_update, led_570.get_grb_word());
    put_pixel(sm_to_update, led_571.get_grb_word());
    put_pixel(sm_to_update, led_572.get_grb_word());
    put_pixel(sm_to_update, led_573.get_grb_word());
    put_pixel(sm_to_update, led_574.get_grb_word());
    put_pixel(sm_to_update, led_575.get_grb_word());
    put_pixel(sm_to_update, led_576.get_grb_word());
    put_pixel(sm_to_update, led_577.get_grb_word());
    put_pixel(sm_to_update, led_578.get_grb_word());
    put_pixel(sm_to_update, led_579.get_grb_word());
    put_pixel(sm_to_update, led_580.get_grb_word());
    put_pixel(sm_to_update, led_581.get_grb_word());
    put_pixel(sm_to_update, led_582.get_grb_word());
    put_pixel(sm_to_update, led_583.get_grb_word());
    put_pixel(sm_to_update, led_584.get_grb_word());
    put_pixel(sm_to_update, led_585.get_grb_word());
    put_pixel(sm_to_update, led_586.get_grb_word());
    put_pixel(sm_to_update, led_587.get_grb_word());
    put_pixel(sm_to_update, led_588.get_grb_word());
    put_pixel(sm_to_update, led_589.get_grb_word());
    put_pixel(sm_to_update, led_590.get_grb_word());
    put_pixel(sm_to_update, led_591.get_grb_word());
    put_pixel(sm_to_update, led_592.get_grb_word());
    put_pixel(sm_to_update, led_593.get_grb_word());
    put_pixel(sm_to_update, led_594.get_grb_word());
    put_pixel(sm_to_update, led_595.get_grb_word());
    put_pixel(sm_to_update, led_596.get_grb_word());
    put_pixel(sm_to_update, led_597.get_grb_word());
    put_pixel(sm_to_update, led_598.get_grb_word());
    put_pixel(sm_to_update, led_599.get_grb_word());
    put_pixel(sm_to_update, led_600.get_grb_word());
    }
    else if (sm_to_update == 2)
    {
    put_pixel(sm_to_update, led_601.get_grb_word());
    put_pixel(sm_to_update, led_602.get_grb_word());
    put_pixel(sm_to_update, led_603.get_grb_word());
    put_pixel(sm_to_update, led_604.get_grb_word());
    put_pixel(sm_to_update, led_605.get_grb_word());
    put_pixel(sm_to_update, led_606.get_grb_word());
    put_pixel(sm_to_update, led_607.get_grb_word());
    put_pixel(sm_to_update, led_608.get_grb_word());
    put_pixel(sm_to_update, led_609.get_grb_word());
    put_pixel(sm_to_update, led_610.get_grb_word());
    put_pixel(sm_to_update, led_611.get_grb_word());
    put_pixel(sm_to_update, led_612.get_grb_word());
    put_pixel(sm_to_update, led_613.get_grb_word());
    put_pixel(sm_to_update, led_614.get_grb_word());
    put_pixel(sm_to_update, led_615.get_grb_word());
    put_pixel(sm_to_update, led_616.get_grb_word());
    put_pixel(sm_to_update, led_617.get_grb_word());
    put_pixel(sm_to_update, led_618.get_grb_word());
    put_pixel(sm_to_update, led_619.get_grb_word());
    put_pixel(sm_to_update, led_620.get_grb_word());
    put_pixel(sm_to_update, led_621.get_grb_word());
    put_pixel(sm_to_update, led_622.get_grb_word());
    put_pixel(sm_to_update, led_623.get_grb_word());
    put_pixel(sm_to_update, led_624.get_grb_word());
    put_pixel(sm_to_update, led_625.get_grb_word());
    put_pixel(sm_to_update, led_626.get_grb_word());
    put_pixel(sm_to_update, led_627.get_grb_word());
    put_pixel(sm_to_update, led_628.get_grb_word());
    put_pixel(sm_to_update, led_629.get_grb_word());
    put_pixel(sm_to_update, led_630.get_grb_word());
    put_pixel(sm_to_update, led_631.get_grb_word());
    put_pixel(sm_to_update, led_632.get_grb_word());
    put_pixel(sm_to_update, led_633.get_grb_word());
    put_pixel(sm_to_update, led_634.get_grb_word());
    put_pixel(sm_to_update, led_635.get_grb_word());
    put_pixel(sm_to_update, led_636.get_grb_word());
    put_pixel(sm_to_update, led_637.get_grb_word());
    put_pixel(sm_to_update, led_638.get_grb_word());
    put_pixel(sm_to_update, led_639.get_grb_word());
    put_pixel(sm_to_update, led_640.get_grb_word());
    put_pixel(sm_to_update, led_641.get_grb_word());
    put_pixel(sm_to_update, led_642.get_grb_word());
    put_pixel(sm_to_update, led_643.get_grb_word());
    put_pixel(sm_to_update, led_644.get_grb_word());
    put_pixel(sm_to_update, led_645.get_grb_word());
    put_pixel(sm_to_update, led_646.get_grb_word());
    put_pixel(sm_to_update, led_647.get_grb_word());
    put_pixel(sm_to_update, led_648.get_grb_word());
    put_pixel(sm_to_update, led_649.get_grb_word());
    put_pixel(sm_to_update, led_650.get_grb_word());
    put_pixel(sm_to_update, led_651.get_grb_word());
    put_pixel(sm_to_update, led_652.get_grb_word());
    put_pixel(sm_to_update, led_653.get_grb_word());
    put_pixel(sm_to_update, led_654.get_grb_word());
    put_pixel(sm_to_update, led_655.get_grb_word());
    put_pixel(sm_to_update, led_656.get_grb_word());
    put_pixel(sm_to_update, led_657.get_grb_word());
    put_pixel(sm_to_update, led_658.get_grb_word());
    put_pixel(sm_to_update, led_659.get_grb_word());
    put_pixel(sm_to_update, led_660.get_grb_word());
    put_pixel(sm_to_update, led_661.get_grb_word());
    put_pixel(sm_to_update, led_662.get_grb_word());
    put_pixel(sm_to_update, led_663.get_grb_word());
    put_pixel(sm_to_update, led_664.get_grb_word());
    put_pixel(sm_to_update, led_665.get_grb_word());
    put_pixel(sm_to_update, led_666.get_grb_word());
    put_pixel(sm_to_update, led_667.get_grb_word());
    put_pixel(sm_to_update, led_668.get_grb_word());
    put_pixel(sm_to_update, led_669.get_grb_word());
    put_pixel(sm_to_update, led_670.get_grb_word());
    put_pixel(sm_to_update, led_671.get_grb_word());
    put_pixel(sm_to_update, led_672.get_grb_word());
    put_pixel(sm_to_update, led_673.get_grb_word());
    put_pixel(sm_to_update, led_674.get_grb_word());
    put_pixel(sm_to_update, led_675.get_grb_word());
    put_pixel(sm_to_update, led_676.get_grb_word());
    put_pixel(sm_to_update, led_677.get_grb_word());
    put_pixel(sm_to_update, led_678.get_grb_word());
    put_pixel(sm_to_update, led_679.get_grb_word());
    put_pixel(sm_to_update, led_680.get_grb_word());
    put_pixel(sm_to_update, led_681.get_grb_word());
    put_pixel(sm_to_update, led_682.get_grb_word());
    put_pixel(sm_to_update, led_683.get_grb_word());
    put_pixel(sm_to_update, led_684.get_grb_word());
    put_pixel(sm_to_update, led_685.get_grb_word());
    put_pixel(sm_to_update, led_686.get_grb_word());
    put_pixel(sm_to_update, led_687.get_grb_word());
    put_pixel(sm_to_update, led_688.get_grb_word());
    put_pixel(sm_to_update, led_689.get_grb_word());
    put_pixel(sm_to_update, led_690.get_grb_word());
    put_pixel(sm_to_update, led_691.get_grb_word());
    put_pixel(sm_to_update, led_692.get_grb_word());
    put_pixel(sm_to_update, led_693.get_grb_word());
    put_pixel(sm_to_update, led_694.get_grb_word());
    put_pixel(sm_to_update, led_695.get_grb_word());
    put_pixel(sm_to_update, led_696.get_grb_word());
    put_pixel(sm_to_update, led_697.get_grb_word());
    put_pixel(sm_to_update, led_698.get_grb_word());
    put_pixel(sm_to_update, led_699.get_grb_word());
    put_pixel(sm_to_update, led_700.get_grb_word());
    put_pixel(sm_to_update, led_701.get_grb_word());
    put_pixel(sm_to_update, led_702.get_grb_word());
    put_pixel(sm_to_update, led_703.get_grb_word());
    put_pixel(sm_to_update, led_704.get_grb_word());
    put_pixel(sm_to_update, led_705.get_grb_word());
    put_pixel(sm_to_update, led_706.get_grb_word());
    put_pixel(sm_to_update, led_707.get_grb_word());
    put_pixel(sm_to_update, led_708.get_grb_word());
    put_pixel(sm_to_update, led_709.get_grb_word());
    put_pixel(sm_to_update, led_710.get_grb_word());
    put_pixel(sm_to_update, led_711.get_grb_word());
    put_pixel(sm_to_update, led_712.get_grb_word());
    put_pixel(sm_to_update, led_713.get_grb_word());
    put_pixel(sm_to_update, led_714.get_grb_word());
    put_pixel(sm_to_update, led_715.get_grb_word());
    put_pixel(sm_to_update, led_716.get_grb_word());
    put_pixel(sm_to_update, led_717.get_grb_word());
    put_pixel(sm_to_update, led_718.get_grb_word());
    put_pixel(sm_to_update, led_719.get_grb_word());
    put_pixel(sm_to_update, led_720.get_grb_word());
    put_pixel(sm_to_update, led_721.get_grb_word());
    put_pixel(sm_to_update, led_722.get_grb_word());
    put_pixel(sm_to_update, led_723.get_grb_word());
    put_pixel(sm_to_update, led_724.get_grb_word());
    put_pixel(sm_to_update, led_725.get_grb_word());
    put_pixel(sm_to_update, led_726.get_grb_word());
    put_pixel(sm_to_update, led_727.get_grb_word());
    put_pixel(sm_to_update, led_728.get_grb_word());
    put_pixel(sm_to_update, led_729.get_grb_word());
    put_pixel(sm_to_update, led_730.get_grb_word());
    put_pixel(sm_to_update, led_731.get_grb_word());
    put_pixel(sm_to_update, led_732.get_grb_word());
    put_pixel(sm_to_update, led_733.get_grb_word());
    put_pixel(sm_to_update, led_734.get_grb_word());
    put_pixel(sm_to_update, led_735.get_grb_word());
    put_pixel(sm_to_update, led_736.get_grb_word());
    put_pixel(sm_to_update, led_737.get_grb_word());
    put_pixel(sm_to_update, led_738.get_grb_word());
    put_pixel(sm_to_update, led_739.get_grb_word());
    put_pixel(sm_to_update, led_740.get_grb_word());
    put_pixel(sm_to_update, led_741.get_grb_word());
    put_pixel(sm_to_update, led_742.get_grb_word());
    put_pixel(sm_to_update, led_743.get_grb_word());
    put_pixel(sm_to_update, led_744.get_grb_word());
    put_pixel(sm_to_update, led_745.get_grb_word());
    put_pixel(sm_to_update, led_746.get_grb_word());
    put_pixel(sm_to_update, led_747.get_grb_word());
    put_pixel(sm_to_update, led_748.get_grb_word());
    put_pixel(sm_to_update, led_749.get_grb_word());
    put_pixel(sm_to_update, led_750.get_grb_word());
    put_pixel(sm_to_update, led_751.get_grb_word());
    put_pixel(sm_to_update, led_752.get_grb_word());
    put_pixel(sm_to_update, led_753.get_grb_word());
    put_pixel(sm_to_update, led_754.get_grb_word());
    put_pixel(sm_to_update, led_755.get_grb_word());
    put_pixel(sm_to_update, led_756.get_grb_word());
    put_pixel(sm_to_update, led_757.get_grb_word());
    put_pixel(sm_to_update, led_758.get_grb_word());
    put_pixel(sm_to_update, led_759.get_grb_word());
    put_pixel(sm_to_update, led_760.get_grb_word());
    put_pixel(sm_to_update, led_761.get_grb_word());
    put_pixel(sm_to_update, led_762.get_grb_word());
    put_pixel(sm_to_update, led_763.get_grb_word());
    put_pixel(sm_to_update, led_764.get_grb_word());
    put_pixel(sm_to_update, led_765.get_grb_word());
    put_pixel(sm_to_update, led_766.get_grb_word());
    put_pixel(sm_to_update, led_767.get_grb_word());
    put_pixel(sm_to_update, led_768.get_grb_word());
    put_pixel(sm_to_update, led_769.get_grb_word());
    put_pixel(sm_to_update, led_770.get_grb_word());
    put_pixel(sm_to_update, led_771.get_grb_word());
    put_pixel(sm_to_update, led_772.get_grb_word());
    put_pixel(sm_to_update, led_773.get_grb_word());
    put_pixel(sm_to_update, led_774.get_grb_word());
    put_pixel(sm_to_update, led_775.get_grb_word());
    put_pixel(sm_to_update, led_776.get_grb_word());
    put_pixel(sm_to_update, led_777.get_grb_word());
    put_pixel(sm_to_update, led_778.get_grb_word());
    put_pixel(sm_to_update, led_779.get_grb_word());
    put_pixel(sm_to_update, led_780.get_grb_word());
    put_pixel(sm_to_update, led_781.get_grb_word());
    put_pixel(sm_to_update, led_782.get_grb_word());
    put_pixel(sm_to_update, led_783.get_grb_word());
    put_pixel(sm_to_update, led_784.get_grb_word());
    put_pixel(sm_to_update, led_785.get_grb_word());
    put_pixel(sm_to_update, led_786.get_grb_word());
    put_pixel(sm_to_update, led_787.get_grb_word());
    put_pixel(sm_to_update, led_788.get_grb_word());
    put_pixel(sm_to_update, led_789.get_grb_word());
    put_pixel(sm_to_update, led_790.get_grb_word());
    put_pixel(sm_to_update, led_791.get_grb_word());
    put_pixel(sm_to_update, led_792.get_grb_word());
    put_pixel(sm_to_update, led_793.get_grb_word());
    put_pixel(sm_to_update, led_794.get_grb_word());
    put_pixel(sm_to_update, led_795.get_grb_word());
    put_pixel(sm_to_update, led_796.get_grb_word());
    put_pixel(sm_to_update, led_797.get_grb_word());
    put_pixel(sm_to_update, led_798.get_grb_word());
    put_pixel(sm_to_update, led_799.get_grb_word());
    put_pixel(sm_to_update, led_800.get_grb_word());
    }
    else if (sm_to_update == 3)
    {
    put_pixel(sm_to_update, led_801.get_grb_word());
    put_pixel(sm_to_update, led_802.get_grb_word());
    put_pixel(sm_to_update, led_803.get_grb_word());
    put_pixel(sm_to_update, led_804.get_grb_word());
    put_pixel(sm_to_update, led_805.get_grb_word());
    put_pixel(sm_to_update, led_806.get_grb_word());
    put_pixel(sm_to_update, led_807.get_grb_word());
    put_pixel(sm_to_update, led_808.get_grb_word());
    put_pixel(sm_to_update, led_809.get_grb_word());
    put_pixel(sm_to_update, led_810.get_grb_word());
    put_pixel(sm_to_update, led_811.get_grb_word());
    put_pixel(sm_to_update, led_812.get_grb_word());
    put_pixel(sm_to_update, led_813.get_grb_word());
    put_pixel(sm_to_update, led_814.get_grb_word());
    put_pixel(sm_to_update, led_815.get_grb_word());
    put_pixel(sm_to_update, led_816.get_grb_word());
    put_pixel(sm_to_update, led_817.get_grb_word());
    put_pixel(sm_to_update, led_818.get_grb_word());
    put_pixel(sm_to_update, led_819.get_grb_word());
    put_pixel(sm_to_update, led_820.get_grb_word());
    put_pixel(sm_to_update, led_821.get_grb_word());
    put_pixel(sm_to_update, led_822.get_grb_word());
    put_pixel(sm_to_update, led_823.get_grb_word());
    put_pixel(sm_to_update, led_824.get_grb_word());
    put_pixel(sm_to_update, led_825.get_grb_word());
    put_pixel(sm_to_update, led_826.get_grb_word());
    put_pixel(sm_to_update, led_827.get_grb_word());
    put_pixel(sm_to_update, led_828.get_grb_word());
    put_pixel(sm_to_update, led_829.get_grb_word());
    put_pixel(sm_to_update, led_830.get_grb_word());
    put_pixel(sm_to_update, led_831.get_grb_word());
    put_pixel(sm_to_update, led_832.get_grb_word());
    put_pixel(sm_to_update, led_833.get_grb_word());
    put_pixel(sm_to_update, led_834.get_grb_word());
    put_pixel(sm_to_update, led_835.get_grb_word());
    put_pixel(sm_to_update, led_836.get_grb_word());
    put_pixel(sm_to_update, led_837.get_grb_word());
    put_pixel(sm_to_update, led_838.get_grb_word());
    put_pixel(sm_to_update, led_839.get_grb_word());
    put_pixel(sm_to_update, led_840.get_grb_word());
    put_pixel(sm_to_update, led_841.get_grb_word());
    put_pixel(sm_to_update, led_842.get_grb_word());
    put_pixel(sm_to_update, led_843.get_grb_word());
    put_pixel(sm_to_update, led_844.get_grb_word());
    put_pixel(sm_to_update, led_845.get_grb_word());
    put_pixel(sm_to_update, led_846.get_grb_word());
    put_pixel(sm_to_update, led_847.get_grb_word());
    put_pixel(sm_to_update, led_848.get_grb_word());
    put_pixel(sm_to_update, led_849.get_grb_word());
    put_pixel(sm_to_update, led_850.get_grb_word());
    put_pixel(sm_to_update, led_851.get_grb_word());
    put_pixel(sm_to_update, led_852.get_grb_word());
    put_pixel(sm_to_update, led_853.get_grb_word());
    put_pixel(sm_to_update, led_854.get_grb_word());
    put_pixel(sm_to_update, led_855.get_grb_word());
    put_pixel(sm_to_update, led_856.get_grb_word());
    put_pixel(sm_to_update, led_857.get_grb_word());
    put_pixel(sm_to_update, led_858.get_grb_word());
    put_pixel(sm_to_update, led_859.get_grb_word());
    put_pixel(sm_to_update, led_860.get_grb_word());
    put_pixel(sm_to_update, led_861.get_grb_word());
    put_pixel(sm_to_update, led_862.get_grb_word());
    put_pixel(sm_to_update, led_863.get_grb_word());
    put_pixel(sm_to_update, led_864.get_grb_word());
    put_pixel(sm_to_update, led_865.get_grb_word());
    put_pixel(sm_to_update, led_866.get_grb_word());
    put_pixel(sm_to_update, led_867.get_grb_word());
    put_pixel(sm_to_update, led_868.get_grb_word());
    put_pixel(sm_to_update, led_869.get_grb_word());
    put_pixel(sm_to_update, led_870.get_grb_word());
    put_pixel(sm_to_update, led_871.get_grb_word());
    put_pixel(sm_to_update, led_872.get_grb_word());
    put_pixel(sm_to_update, led_873.get_grb_word());
    put_pixel(sm_to_update, led_874.get_grb_word());
    put_pixel(sm_to_update, led_875.get_grb_word());
    put_pixel(sm_to_update, led_876.get_grb_word());
    put_pixel(sm_to_update, led_877.get_grb_word());
    put_pixel(sm_to_update, led_878.get_grb_word());
    put_pixel(sm_to_update, led_879.get_grb_word());
    put_pixel(sm_to_update, led_880.get_grb_word());
    put_pixel(sm_to_update, led_881.get_grb_word());
    put_pixel(sm_to_update, led_882.get_grb_word());
    put_pixel(sm_to_update, led_883.get_grb_word());
    put_pixel(sm_to_update, led_884.get_grb_word());
    put_pixel(sm_to_update, led_885.get_grb_word());
    put_pixel(sm_to_update, led_886.get_grb_word());
    put_pixel(sm_to_update, led_887.get_grb_word());
    put_pixel(sm_to_update, led_888.get_grb_word());
    put_pixel(sm_to_update, led_889.get_grb_word());
    put_pixel(sm_to_update, led_890.get_grb_word());
    put_pixel(sm_to_update, led_891.get_grb_word());
    put_pixel(sm_to_update, led_892.get_grb_word());
    put_pixel(sm_to_update, led_893.get_grb_word());
    put_pixel(sm_to_update, led_894.get_grb_word());
    put_pixel(sm_to_update, led_895.get_grb_word());
    put_pixel(sm_to_update, led_896.get_grb_word());
    put_pixel(sm_to_update, led_897.get_grb_word());
    put_pixel(sm_to_update, led_898.get_grb_word());
    put_pixel(sm_to_update, led_899.get_grb_word());
    put_pixel(sm_to_update, led_900.get_grb_word());
    put_pixel(sm_to_update, led_901.get_grb_word());
    put_pixel(sm_to_update, led_902.get_grb_word());
    put_pixel(sm_to_update, led_903.get_grb_word());
    put_pixel(sm_to_update, led_904.get_grb_word());
    put_pixel(sm_to_update, led_905.get_grb_word());
    put_pixel(sm_to_update, led_906.get_grb_word());
    put_pixel(sm_to_update, led_907.get_grb_word());
    put_pixel(sm_to_update, led_908.get_grb_word());
    put_pixel(sm_to_update, led_909.get_grb_word());
    put_pixel(sm_to_update, led_910.get_grb_word());
    put_pixel(sm_to_update, led_911.get_grb_word());
    put_pixel(sm_to_update, led_912.get_grb_word());
    put_pixel(sm_to_update, led_913.get_grb_word());
    put_pixel(sm_to_update, led_914.get_grb_word());
    put_pixel(sm_to_update, led_915.get_grb_word());
    put_pixel(sm_to_update, led_916.get_grb_word());
    put_pixel(sm_to_update, led_917.get_grb_word());
    put_pixel(sm_to_update, led_918.get_grb_word());
    put_pixel(sm_to_update, led_919.get_grb_word());
    put_pixel(sm_to_update, led_920.get_grb_word());
    put_pixel(sm_to_update, led_921.get_grb_word());
    put_pixel(sm_to_update, led_922.get_grb_word());
    put_pixel(sm_to_update, led_923.get_grb_word());
    put_pixel(sm_to_update, led_924.get_grb_word());
    put_pixel(sm_to_update, led_925.get_grb_word());
    put_pixel(sm_to_update, led_926.get_grb_word());
    put_pixel(sm_to_update, led_927.get_grb_word());
    put_pixel(sm_to_update, led_928.get_grb_word());
    put_pixel(sm_to_update, led_929.get_grb_word());
    put_pixel(sm_to_update, led_930.get_grb_word());
    put_pixel(sm_to_update, led_931.get_grb_word());
    put_pixel(sm_to_update, led_932.get_grb_word());
    put_pixel(sm_to_update, led_933.get_grb_word());
    put_pixel(sm_to_update, led_934.get_grb_word());
    put_pixel(sm_to_update, led_935.get_grb_word());
    put_pixel(sm_to_update, led_936.get_grb_word());
    put_pixel(sm_to_update, led_937.get_grb_word());
    put_pixel(sm_to_update, led_938.get_grb_word());
    put_pixel(sm_to_update, led_939.get_grb_word());
    put_pixel(sm_to_update, led_940.get_grb_word());
    put_pixel(sm_to_update, led_941.get_grb_word());
    put_pixel(sm_to_update, led_942.get_grb_word());
    put_pixel(sm_to_update, led_943.get_grb_word());
    put_pixel(sm_to_update, led_944.get_grb_word());
    put_pixel(sm_to_update, led_945.get_grb_word());
    put_pixel(sm_to_update, led_946.get_grb_word());
    put_pixel(sm_to_update, led_947.get_grb_word());
    put_pixel(sm_to_update, led_948.get_grb_word());
    put_pixel(sm_to_update, led_949.get_grb_word());
    put_pixel(sm_to_update, led_950.get_grb_word());
    put_pixel(sm_to_update, led_951.get_grb_word());
    put_pixel(sm_to_update, led_952.get_grb_word());
    put_pixel(sm_to_update, led_953.get_grb_word());
    put_pixel(sm_to_update, led_954.get_grb_word());
    put_pixel(sm_to_update, led_955.get_grb_word());
    put_pixel(sm_to_update, led_956.get_grb_word());
    put_pixel(sm_to_update, led_957.get_grb_word());
    put_pixel(sm_to_update, led_958.get_grb_word());
    put_pixel(sm_to_update, led_959.get_grb_word());
    put_pixel(sm_to_update, led_960.get_grb_word());
    put_pixel(sm_to_update, led_961.get_grb_word());
    put_pixel(sm_to_update, led_962.get_grb_word());
    put_pixel(sm_to_update, led_963.get_grb_word());
    put_pixel(sm_to_update, led_964.get_grb_word());
    put_pixel(sm_to_update, led_965.get_grb_word());
    put_pixel(sm_to_update, led_966.get_grb_word());
    put_pixel(sm_to_update, led_967.get_grb_word());
    put_pixel(sm_to_update, led_968.get_grb_word());
    put_pixel(sm_to_update, led_969.get_grb_word());
    put_pixel(sm_to_update, led_970.get_grb_word());
    put_pixel(sm_to_update, led_971.get_grb_word());
    put_pixel(sm_to_update, led_972.get_grb_word());
    put_pixel(sm_to_update, led_973.get_grb_word());
    put_pixel(sm_to_update, led_974.get_grb_word());
    put_pixel(sm_to_update, led_975.get_grb_word());
    put_pixel(sm_to_update, led_976.get_grb_word());
    put_pixel(sm_to_update, led_977.get_grb_word());
    put_pixel(sm_to_update, led_978.get_grb_word());
    put_pixel(sm_to_update, led_979.get_grb_word());
    put_pixel(sm_to_update, led_980.get_grb_word());
    put_pixel(sm_to_update, led_981.get_grb_word());
    put_pixel(sm_to_update, led_982.get_grb_word());
    put_pixel(sm_to_update, led_983.get_grb_word());
    put_pixel(sm_to_update, led_984.get_grb_word());
    put_pixel(sm_to_update, led_985.get_grb_word());
    put_pixel(sm_to_update, led_986.get_grb_word());
    put_pixel(sm_to_update, led_987.get_grb_word());
    put_pixel(sm_to_update, led_988.get_grb_word());
    put_pixel(sm_to_update, led_989.get_grb_word());
    put_pixel(sm_to_update, led_990.get_grb_word());
    put_pixel(sm_to_update, led_991.get_grb_word());
    put_pixel(sm_to_update, led_992.get_grb_word());
    put_pixel(sm_to_update, led_993.get_grb_word());
    put_pixel(sm_to_update, led_994.get_grb_word());
    put_pixel(sm_to_update, led_995.get_grb_word());
    put_pixel(sm_to_update, led_996.get_grb_word());
    put_pixel(sm_to_update, led_997.get_grb_word());
    put_pixel(sm_to_update, led_998.get_grb_word());
    put_pixel(sm_to_update, led_999.get_grb_word());
    put_pixel(sm_to_update, led_1000.get_grb_word());
    }

    restore_interrupts(interrupts);
};

volatile uint32_t string_write_index=0;
void write_my_tree()
{
    //if (string_write_index % 3)
    //{
    //    write_string(0);
    //}
    //else if ((string_write_index + 1) % 3)
    //{
    //    write_string(1);
    //}
    //else if ((string_write_index + 2) % 3)
    //{
    //    write_string(2);
    //}
    //else if ((string_write_index + 3) % 3)
    //{
    //    write_string(3);
    //}
    //string_write_index++;
    write_string(0);
    //sleep_ms(250);
    write_string(1);
    //sleep_ms(250);
    write_string(2);
    //sleep_ms(250);
    write_string(3);
}

uint8_t sleep_val = 25;
// Was 40 - too small to hold a COLOR_GROUP_RGB_UPDATE message (up to
// max_group_update_entries LEDs at 5 bytes each, see dcm_rgb.hpp). Every
// message type shares this one fixed-size buffer, so bumping it just gives
// the smaller messages more headroom - trivial extra RAM (2 buffers x a
// few hundred bytes) on an RP2040.
#define SERIAL_BUFFER_SIZE 256
uint8_t serial_buf[SERIAL_BUFFER_SIZE] = {};    // 
uint8_t serial_buf_copy[SERIAL_BUFFER_SIZE] = {};    // 
uint8_t buf_copy_lock = 0;  // 0 = unlocked, 1 = locked, 2 = ready to read
uint8_t buf_index = 0;
int16_t temp_char = -1; // init to a no bytes value
void serial_read_buffer()
{
    buf_copy_lock = 1;
    while ((temp_char = getchar_timeout_us(0)) >= 0
            && buf_index < SERIAL_BUFFER_SIZE)
    {
        //add to buffer
        serial_buf[buf_index] = temp_char;
        buf_index++;
    }
    if (buf_index != 0)
    {
        // we did read something into buffer
        temp_char = -1; // reset temp_char - don't think it's actually necessary but won't hurt
        memcpy(serial_buf_copy,serial_buf, SERIAL_BUFFER_SIZE);
        memset(serial_buf,0,SERIAL_BUFFER_SIZE);    // zero buffer
        for (size_t i = 0; i < buf_index; i++)
        {
            printf("%d\n", serial_buf_copy[i]);
        }
        temp_char = -1; // reset temp_char - don't think it's actually necessary but won't hurt
        buf_index = 0;  // reset buffer to beginning
        buf_copy_lock = 2;
    }
    else buf_copy_lock = 0;
}
enum class serial_msg_type : uint8_t
{
    NOOP,
    SINGLE_LED_UPDATE,
    COLOR_GROUP_RGB_UPDATE,
    ALL_LED_UPDATE,
    LED_POS_UPDATE_CARTESIAN,
    LED_POS_UPDATE_CYLINDRICAL,
    CONFIG_RELOAD,
    RUN_SWEEP_SEQUENCE,
    // Appended rather than inserted, to keep existing numeric values
    // (and therefore wire compatibility) unchanged.
    READ_POS_CONFIG,
    SET_VOLUME_CARTESIAN,
    SET_VOLUME_CYLINDRICAL,
    // One-shot: overwrites both flash and the live tree with the
    // compiled-in default position config (mapping/
    // generate_pos_config_header.py) - the fast path for pushing a full
    // coordinate update (reflash with freshly generated data, then send
    // this) instead of replaying ~1000 individual position writes.
    RESET_POS_CONFIG_TO_DEFAULT,
    // Sets the PRIMARY/base color for every LED (same payload shape as
    // ALL_LED_UPDATE, which only ever touches the secondary overlay) - the
    // color shown when a LED isn't currently lit by anything else, e.g.
    // outside a SET_VOLUME_* window with clear_outside_volume set.
    ALL_LED_UPDATE_BASE,
};

uint32_t msg_process_counter = 0;
serial_msg_type new_msg = serial_msg_type::NOOP;
struct single_led_update_frame
{
    uint8_t s_msg_type;
    single_led_update_t s_msg;
}__packed;
struct all_led_update_frame
{
    uint8_t s_msg_type;
    all_led_update_t s_msg;
}__packed;
struct single_led_pos_cylindrical_update_frame
{
    uint8_t s_msg_type;
    single_led_pos_cylindrical_update_t s_msg;
}__packed;
struct single_led_pos_cartesian_update_frame
{
    uint8_t s_msg_type;
    single_led_pos_cartesian_update_t s_msg;
}__packed;
struct config_reload_frame
{
    uint8_t s_msg_type;
    config_type s_msg;
}__packed;
struct read_pos_config_request_frame
{
    uint8_t s_msg_type;
    read_pos_config_request_t s_msg;
}__packed;
struct group_led_update_frame
{
    uint8_t s_msg_type;
    group_led_update_t s_msg;
}__packed;
struct set_volume_cartesian_frame
{
    uint8_t s_msg_type;
    set_volume_cartesian_t s_msg;
}__packed;
struct set_volume_cylindrical_frame
{
    uint8_t s_msg_type;
    set_volume_cylindrical_t s_msg;
}__packed;

union single_led_update_msg
{
    uint8_t buf[SERIAL_BUFFER_SIZE];
    single_led_update_frame msg;
};
union all_led_update_msg
{
    uint8_t buf[SERIAL_BUFFER_SIZE];
    all_led_update_frame msg;
};
union single_led_pos_cylindrical_update_msg
{
    uint8_t buf[SERIAL_BUFFER_SIZE];
    single_led_pos_cylindrical_update_frame msg;
};
union single_led_pos_cartesian_update_msg
{
    uint8_t buf[SERIAL_BUFFER_SIZE];
    single_led_pos_cartesian_update_frame msg;
};
union config_reload_msg
{
    uint8_t buf[SERIAL_BUFFER_SIZE];
    config_reload_frame msg;
};
union read_pos_config_request_msg
{
    uint8_t buf[SERIAL_BUFFER_SIZE];
    read_pos_config_request_frame msg;
};
union group_led_update_msg
{
    uint8_t buf[SERIAL_BUFFER_SIZE];
    group_led_update_frame msg;
};
union set_volume_cartesian_msg
{
    uint8_t buf[SERIAL_BUFFER_SIZE];
    set_volume_cartesian_frame msg;
};
union set_volume_cylindrical_msg
{
    uint8_t buf[SERIAL_BUFFER_SIZE];
    set_volume_cylindrical_frame msg;
};

void process_msg()
{
    // call within a buffer lock check
    // First byte is msg_type
    new_msg = static_cast<serial_msg_type>(serial_buf_copy[0]);
/*     if (new_msg == serial_msg_type::SINGLE_LED_UPDATE)
    {
        update_msg temp_update_msg;
        memcpy(temp_update_msg.buf,serial_buf_copy,sizeof(temp_update_msg.buf));    // extra copy fuck it - it works
        //*temp_update_msg.buf = *serial_buf_copy;
        // do update stuff
        RGB_LED_3D::update_single(&(temp_update_msg.msg.s_msg));
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
    }
    else if (new_msg == serial_msg_type::COLOR_GROUP_RGB_UPDATE)
    {
        // don't do shit yet
    }
 */    
    // shouldn't need this, just cast the buffer but w/e fuck it

    single_led_update_msg temp_update_msg;
    all_led_update_msg temp_all_led_update_msg;
    single_led_pos_cylindrical_update_msg temp_single_led_pos_cylindrical_update_msg;
    single_led_pos_cartesian_update_msg temp_single_led_pos_cartesian_update_msg;
    config_reload_msg temp_config_reload_msg;
    read_pos_config_request_msg temp_read_pos_config_msg;
    group_led_update_msg temp_group_led_update_msg;
    set_volume_cartesian_msg temp_set_volume_cartesian_msg;
    set_volume_cylindrical_msg temp_set_volume_cylindrical_msg;
    switch (new_msg)
    {
    case serial_msg_type::NOOP:
        // Don't need to do shit, but can do a serial print to show it worked
        printf("NOOP received ");
        break;
    case serial_msg_type::SINGLE_LED_UPDATE:
        memcpy(temp_update_msg.buf,serial_buf_copy,sizeof(temp_update_msg.buf));    // extra copy fuck it - it works
        // do update stuff
        RGB_LED_3D::update_single(&(temp_update_msg.msg.s_msg));
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;
    case serial_msg_type::COLOR_GROUP_RGB_UPDATE:
        memcpy(temp_group_led_update_msg.buf,serial_buf_copy,sizeof(temp_update_msg.buf));    // extra copy fuck it - it works
        RGB_LED_3D::update_group(&(temp_group_led_update_msg.msg.s_msg));
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;
    case serial_msg_type::ALL_LED_UPDATE:
        memcpy(temp_all_led_update_msg.buf,serial_buf_copy,sizeof(temp_update_msg.buf));    // extra copy fuck it - it works
        // do update stuff
        RGB_LED_3D::update_ALL(&(temp_all_led_update_msg.msg.s_msg));
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;
    case serial_msg_type::LED_POS_UPDATE_CARTESIAN:
        printf("LED_POS_UPDATE_CARTESIAN PROCESSING ");
        memcpy(temp_single_led_pos_cartesian_update_msg.buf,serial_buf_copy,sizeof(temp_update_msg.buf));    // extra copy fuck it - it works
        // do update stuff
        // config coordinates are stored as cylindrical - need to convert first
        printf("convert coordinates to cylindrical ");
        temp_single_led_pos_cylindrical_update_msg.msg.s_msg.rgb_update = transform_cartesian_to_cylindrical(temp_single_led_pos_cartesian_update_msg.msg.s_msg.rgb_update);
        temp_single_led_pos_cylindrical_update_msg.msg.s_msg.led_string_position = temp_single_led_pos_cartesian_update_msg.msg.s_msg.led_string_position;
        printf("attempt write_flash_pos_config ");
        write_flash_pos_config(*reinterpret_cast<string_led_config*>(&temp_single_led_pos_cylindrical_update_msg.msg.s_msg));
        // Read back and print the actual stored result, independent of
        // write_flash_pos_config()'s own internal verify check - lets a
        // caller (or a human on a serial monitor) confirm the write really
        // took by eye, not just trust a pass/fail flag.
        print_pos_config(temp_single_led_pos_cylindrical_update_msg.msg.s_msg.led_string_position);
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;
    case serial_msg_type::LED_POS_UPDATE_CYLINDRICAL:
        printf("LED_POS_UPDATE_CYLINDRICAL PROCESSING ");
        memcpy(temp_single_led_pos_cylindrical_update_msg.buf,serial_buf_copy,sizeof(temp_update_msg.buf));    // extra copy fuck it - it works
        // do update stuff
        printf("attempt write_flash_pos_config ");
        write_flash_pos_config(*reinterpret_cast<string_led_config*>(&temp_single_led_pos_cylindrical_update_msg.msg.s_msg));
        print_pos_config(temp_single_led_pos_cylindrical_update_msg.msg.s_msg.led_string_position);
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;
    case serial_msg_type::READ_POS_CONFIG:
        memcpy(temp_read_pos_config_msg.buf,serial_buf_copy,sizeof(temp_update_msg.buf));    // extra copy fuck it - it works
        print_pos_config(temp_read_pos_config_msg.msg.s_msg.led_string_position);
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;
    case serial_msg_type::SET_VOLUME_CARTESIAN:
        memcpy(temp_set_volume_cartesian_msg.buf,serial_buf_copy,sizeof(temp_update_msg.buf));    // extra copy fuck it - it works
        RGB_LED_3D::update_volume_cartesian(&(temp_set_volume_cartesian_msg.msg.s_msg));
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;
    case serial_msg_type::SET_VOLUME_CYLINDRICAL:
        memcpy(temp_set_volume_cylindrical_msg.buf,serial_buf_copy,sizeof(temp_update_msg.buf));    // extra copy fuck it - it works
        RGB_LED_3D::update_volume_cylindrical(&(temp_set_volume_cylindrical_msg.msg.s_msg));
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;
    case serial_msg_type::CONFIG_RELOAD:
        memcpy(temp_config_reload_msg.buf,serial_buf_copy,sizeof(temp_update_msg.buf));    // extra copy fuck it - it works
        // do update stuff
        RGB_LED_3D::initialize_from_config();
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;
    case serial_msg_type::RESET_POS_CONFIG_TO_DEFAULT:
        reset_pos_config_to_default();
        RGB_LED_3D::initialize_from_config();
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;
    case serial_msg_type::ALL_LED_UPDATE_BASE:
        memcpy(temp_all_led_update_msg.buf,serial_buf_copy,sizeof(temp_update_msg.buf));    // extra copy fuck it - it works
        RGB_LED_3D::update_ALL_base(&(temp_all_led_update_msg.msg.s_msg));
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;

    default:
        // not a valid msg_type
        printf("Not a valid msg type");
        break;
    }
    
}

void main_core1()
{
    // code for second core
    // Register this core as a lockout "victim" so core0 can pause it via
    // multicore_lockout_start/end_blocking() during flash erase/program in
    // write_flash_pos_config() (neo_tree_config.cpp). This core spins in
    // the serial-read loop below continuously, executing from flash (XIP)
    // the whole time - flash erase/program is documented as unsafe unless
    // the other core is prevented from fetching from flash concurrently.
    multicore_lockout_victim_init();
    // cyw43_arch_init() talks to the onboard CYW43 WiFi/BT chip, which on
    // this board is unreliable - confirmed (via added diagnostics) to
    // sometimes hang indefinitely and never return at all. Since it blocks
    // at the very top of this function, before the serial-reading loop
    // below, a hang here means core1 never reads serial for the rest of
    // the session - which is exactly the bug this was all chasing. WiFi
    // isn't used for anything today (lwIP/sta-mode are already commented
    // out below), so skip the call entirely rather than depend on a chip
    // that doesn't reliably come up.
    bool wifi_ok = false;
    //cyw43_arch_enable_sta_mode();

/*     if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000)) {
        printf("failed to connect\n");
    }
 */
    // Watchdog heartbeat for core1 itself, mirroring the core0 one, so we
    // can see whether this loop is actually cycling (and how fast) once
    // past init, independent of whether any serial data ever arrives.
    uint64_t last_core1_heartbeat_us = 0;
    const uint64_t core1_heartbeat_interval_us = 5'000'000;
    uint64_t core1_loop_count = 0;
    while (true) {
        core1_loop_count++;
        uint64_t now_us = time_us_64();
        if (now_us > (last_core1_heartbeat_us + core1_heartbeat_interval_us))
        {
            last_core1_heartbeat_us = now_us;
            printf("core1 alive: loop_count=%u buf_copy_lock=%d\n", (uint32_t)core1_loop_count, (int)buf_copy_lock);
        }
        if (wifi_ok)
        {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
        }
        // serial time - doing with LED on
        if (buf_copy_lock == 0)
        {
            serial_read_buffer();
        }
        if (wifi_ok)
        {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
        }
    }
}

uint32_t target_loop_rate = 60;
volatile uint64_t initial_abs_time_check = 0;
uint64_t initial_startup_delay = 1'500'00;
volatile uint64_t latest_abs_time_check = 0;
uint64_t led_loop_counter = 0;
const uint32_t loop_duration_micros = 1'000'000 / target_loop_rate;

// Watchdog heartbeat - prints uptime over serial every 5s regardless of
// whether any command has been received, so liveness of the main core0
// loop can be confirmed independent of the serial link.
volatile uint64_t last_uptime_print_us = 0;
const uint64_t uptime_print_interval_us = 5'000'000;   // 5 seconds

int main() {
    //set_sys_clock_48();
    stdio_init_all();
    multicore_launch_core1(main_core1);

    printf("WS2812 Smoke Test, using pin %d", WS2812_PIN_STRING_1);
    // Deploy-pipeline proof marker - a fresh, one-off token picked at the
    // time this change was written, not derived from anything already on
    // the board, so seeing it over serial after a deploy proves this exact
    // build is what's actually running.
    printf("\nDEPLOY MARKER: 081f4c04\n");

    // todo get free sm
    PIO pio = pio0;
    uint offset = pio_add_program(pio, &ws2812_program);
    ws2812_program_init(pio, 0, offset, WS2812_PIN_STRING_1, 800000, IS_RGBW);
    ws2812_program_init(pio, 1, offset, WS2812_PIN_STRING_2, 800000, IS_RGBW);
    ws2812_program_init(pio, 2, offset, WS2812_PIN_STRING_3, 800000, IS_RGBW);
    ws2812_program_init(pio, 3, offset, WS2812_PIN_STRING_4, 800000, IS_RGBW);

    init_my_tree();
    // Load any persisted LED position config from flash (falls back to
    // compiled-in defaults if flash doesn't hold a valid config yet) and
    // apply it to the tree. Previously this only happened on an explicit
    // CONFIG_RELOAD serial command, so even a correctly-written config
    // would have had no effect after a real power cycle - the write path
    // existed but nothing ever loaded it back.
    load_pos_config_from_flash();
    RGB_LED_3D::initialize_from_config();
    // grab first loop a abs time
    initial_abs_time_check = get_absolute_time();
    // adding a wait loop before starting the main while loop
    // this was necessary to stop the neopixel data from glitching out from something fucking up at startup
    // not a root cause solution, band-aid and lgtm
    while(get_absolute_time() < (initial_abs_time_check + initial_startup_delay)){;}
    //redo initial time check for loop timer
    initial_abs_time_check = get_absolute_time();
    while(1)
    {
        latest_abs_time_check = get_absolute_time();
        if (latest_abs_time_check > (last_uptime_print_us + uptime_print_interval_us))
        {
            last_uptime_print_us = latest_abs_time_check;
            printf("uptime s: %u marker: 081f4c04 led_loop_counter: %u\n",
                   (uint32_t)(latest_abs_time_check / 1'000'000), (uint32_t)led_loop_counter);
        }
        if (buf_copy_lock == 2)
        {
            process_msg();
            buf_copy_lock = 0;
        }
        if (latest_abs_time_check > (initial_abs_time_check + (loop_duration_micros * led_loop_counter)))
        {
            // time for new led loop
            led_loop_counter++;
            write_my_tree();
            //printf("led_loop_counter: ");
            //printf("%d\n", (uint32_t)led_loop_counter);
            //sleep_ms(10);
            //sleep_ms(100000000);
        }
    }

}
