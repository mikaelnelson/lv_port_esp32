/**
 * @file ra8875.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "ra8875.h"
#include "disp_spi.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*********************
 *      DEFINES
 *********************/
#define DEBUG false
#define TAG "RA8875"

#define DIV_ROUND_UP(n, d) (((n)+(d)-1)/(d))

#define SPI_CLOCK_SPEED_SLOW_HZ 1000000

#define RA8875_MODE_DATA_WRITE  (0x00)
#define RA8875_MODE_DATA_READ   (0x40)
#define RA8875_MODE_CMD_WRITE   (0x80)
#define RA8875_MODE_STATUS_READ (0xC0)

#if (LV_COLOR_DEPTH == 8)
    #define SYSR_VAL (0x00)
#elif (LV_COLOR_DEPTH == 16)
    #define SYSR_VAL (0x08)
#else
    #error "Unsupported color depth (LV_COLOR_DEPTH)"
#endif
#define BYTES_PER_PIXEL (LV_COLOR_DEPTH / 8)

#define HDWR_VAL (CONFIG_LVGL_DISPLAY_WIDTH/8 - 1)
#define VDHR_VAL 144//(CONFIG_LVGL_DISPLAY_HEIGHT - 1)

#define VDIR_MASK (1 << 2)
#define HDIR_MASK (1 << 3)

#if ( CONFIG_LVGL_DISPLAY_ORIENTATION_PORTRAIT_INVERTED || CONFIG_LVGL_DISPLAY_ORIENTATION_LANDSCAPE_INVERTED )
    #if CONFIG_LVGL_INVERT_DISPLAY
        #define DPCR_VAL (VDIR_MASK)
    #else
        #define DPCR_VAL (VDIR_MASK | HDIR_MASK)
    #endif
#else
    #if CONFIG_LVGL_INVERT_DISPLAY
        #define DPCR_VAL (HDIR_MASK)
    #else
        #define DPCR_VAL (0x00)
    #endif
#endif

#if CONFIG_LVGL_DISP_RA8875_PCLK_INVERT
    #define PCSR_VAL (0x80 | CONFIG_LVGL_DISP_RA8875_PCLK_MULTIPLIER)
#else
    #define PCSR_VAL (CONFIG_LVGL_DISP_RA8875_PCLK_MULTIPLIER)
#endif

// Calculate horizontal display parameters
#if (CONFIG_LVGL_DISP_RA8875_HORI_NON_DISP_PERIOD >= 260)
    #define HNDR_VAL (31)
#else
    #define HNDR_VAL ((CONFIG_LVGL_DISP_RA8875_HORI_NON_DISP_PERIOD-12) / 8)
#endif
#define HNDFT (CONFIG_LVGL_DISP_RA8875_HORI_NON_DISP_PERIOD-(8*HNDR_VAL)-12)
#if LVGL_DISP_RA8875_DE_POLARITY
    #define HNDFTR_VAL (0x80 | HNDFT)
#else
    #define HNDFTR_VAL (HNDFT)
#endif
#define HSTR_VAL (CONFIG_LVGL_DISP_RA8875_HSYNC_START/8 - 1)
#define HPW (CONFIG_LVGL_DISP_RA8875_HSYNC_PW/8 - 1)
#if LVGL_DISP_RA8875_HSYNC_POLARITY
    #define HPWR_VAL (0x80 | HPW)
#else
    #define HPWR_VAL (HPW)
#endif

// Calculate vertical display parameters
#define VNDR_VAL (CONFIG_LVGL_DISP_RA8875_VERT_NON_DISP_PERIOD - 1)
#define VSTR_VAL (CONFIG_LVGL_DISP_RA8875_VSYNC_START - 1)
#define VPW (CONFIG_LVGL_DISP_RA8875_VSYNC_PW - 1)
#if LVGL_DISP_RA8875_VSYNC_POLARITY
    #define VPWR_VAL (0x80 | VPW)
#else
    #define VPWR_VAL (VPW)
#endif

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void ra8875_configure_clocks(bool high_speed);
static void ra8875_set_memory_write_cursor(unsigned int x, unsigned int y);
static void ra8875_set_window(unsigned int xs, unsigned int xe, unsigned int ys, unsigned int ye);
static void ra8875_send_buffer(uint8_t * data, size_t length, bool signal_flush);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void ra8875_init(void)
{
    unsigned int i = 0;

    struct {
        uint8_t cmd;                                   // Register address of command
        uint8_t data;                                  // Value to write to register
    } init_cmds[] = {
        {RA8875_REG_SYSR,   0x0C},                 // System Configuration Register (SYSR)
        {RA8875_REG_HDWR,   0x3B},                 // LCD Horizontal Display Width Register (HDWR)
        {RA8875_REG_HNDFTR, 0x00},               // Horizontal Non-Display Period Fine Tuning Option Register (HNDFTR)
        {RA8875_REG_HNDR,   0x01},                 // Horizontal Non-Display Period Register (HNDR)
        {RA8875_REG_HSTR,   0x00},                 // HSYNC Start Position Register (HSTR)
        {RA8875_REG_HPWR,   0x05},                 // HSYNC Pulse Width Register (HPWR)
        {RA8875_REG_VDHR0,  0x0F},         // LCD Vertical Display Height Register (VDHR0)
        {RA8875_REG_VDHR1,  0x01},            // LCD Vertical Display Height Register0 (VDHR1)
        {RA8875_REG_VNDR0,  0x02},         // LCD Vertical Non-Display Period Register (VNDR0)
        {RA8875_REG_VNDR1,  0x00},            // LCD Vertical Non-Display Period Register (VNDR1)
        {RA8875_REG_VSTR0,  0x07},         // VSYNC Start Position Register (VSTR0)
        {RA8875_REG_VSTR1,  0x00},            // VSYNC Start Position Register (VSTR1)
        {RA8875_REG_VPWR,   0x09},                 // VSYNC Pulse Width Register (VPWR)

//#define RA8875_REG_HSAW0  (0x30)     // Horizontal Start Point 0 of Active Window (HSAW0)
//#define RA8875_REG_HSAW1  (0x31)     // Horizontal Start Point 1 of Active Window (HSAW1)
//#define RA8875_REG_VSAW0  (0x32)     // Vertical Start Point 0 of Active Window (VSAW0)
//#define RA8875_REG_VSAW1  (0x33)     // Vertical Start Point 1 of Active Window (VSAW1)
//#define RA8875_REG_HEAW0  (0x34)     // Horizontal End Point 0 of Active Window (HEAW0)
//#define RA8875_REG_HEAW1  (0x35)     // Horizontal End Point 1 of Active Window (HEAW1)
//#define RA8875_REG_VEAW0  (0x36)     // Vertical End Point 0 of Active Window (VEAW0)
//#define RA8875_REG_VEAW1  (0x37)     // Vertical End Point 1 of Active Window (VEAW1)

		{RA8875_REG_HSAW0,   0x00},
		{RA8875_REG_HSAW1,   0x00},
		{RA8875_REG_HEAW0,   0xDF},
		{RA8875_REG_HEAW1,   0x01},
		{RA8875_REG_VSAW0,   0x90},
		{RA8875_REG_VSAW1,   0x90},
		{RA8875_REG_VEAW0,   0x0F},
		{RA8875_REG_VEAW1,   0x01},

		{RA8875_REG_MCLR, 0x80},
		{RA8875_REG_PWRR, 0x80},

//			writeReg: 0x80 0xC7 0x0 0x1
//			writeReg: 0x80 0x8A 0x0 0x8A
//			writeReg: 0x80 0x8B 0x0 0xFF

		{0xC7, 0x01},
		{0x8A, 0x8A},
		{0x8B, 0xFF},


//        {RA8875_REG_MWCR0,  0x00},                     // Memory Write Control Register 0 (MWCR0)
//        {RA8875_REG_MWCR1,  0x00},                     // Memory Write Control Register 1 (MWCR1)
//        {RA8875_REG_LTPR0,  0x00},                     // Layer Transparency Register0 (LTPR0)
//        {RA8875_REG_LTPR1,  0x00},                     // Layer Transparency Register1 (LTPR1)
//		{RA8875_REG_VDHR0, 0x0F},
//		{RA8875_REG_VDHR1, 0x01},
//		{RA8875_REG_VSAW0, 0x90},
//		{RA8875_REG_VSAW1, 0x00},
//		{RA8875_REG_VEAW0, 0x01},
//		{RA8875_REG_VEAW0, 0x0F},
//		{RA8875_REG_P1CR, 0x8A},
//		{RA8875_REG_P1DCR, 255},


//		{0x8B,  0xFF},
//		{	0x8B,  0xFA},
//		{	0x8B,  0xF5},
//		{0x8B,  0xF0},
//		{0x8B,  0xEB},
//		{0x8B,  0xE6},
//		{0x8B,  0xE1},
//		{0x8B,  0xDC},

//		{		0x46, 0xA},
//	{			0x47, 0x0},
//{			0x48, 0x9A},
//{			0x49, 0x0},
//{			0x46, 0xB},
//{			0x47, 0x0},
//{			0x48, 0x9B},
//{0x49, 0x0},


/*writeReg:*/ { 0x91, 0xA},
/*writeReg:*/ { 0x92, 0x0},
/*writeReg:*/ { 0x93, 0x9A},
/*writeReg:*/ { 0x94, 0x0},
/*writeReg:*/ { 0x95, 0x99},
/*writeReg:*/ { 0x96, 0x1},
/*writeReg:*/ { 0x97, 0x61},
/*writeReg:*/ { 0x98, 0x1},
/*writeReg:*/ { 0x63, 0x0},
/*writeReg:*/ { 0x64, 0x3F},
/*writeReg:*/ { 0x65, 0x0},
/*writeReg:*/ { 0x90, 0x90},



    };
    #define INIT_CMDS_SIZE (sizeof(init_cmds)/sizeof(init_cmds[0]))

    ESP_LOGI(TAG, "Initializing RA8875...");

#if (CONFIG_LVGL_DISP_PIN_BCKL == 15)
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = GPIO_SEL_15;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
#endif

    // Initialize non-SPI GPIOs
    gpio_set_direction(RA8875_RST, GPIO_MODE_OUTPUT);
#ifdef CONFIG_LVGL_DISP_PIN_BCKL
    gpio_set_direction(CONFIG_LVGL_DISP_PIN_BCKL, GPIO_MODE_OUTPUT);
#endif

    // Reset the RA8875
    gpio_set_level(RA8875_RST, 0);
    vTaskDelay(DIV_ROUND_UP(100, portTICK_RATE_MS));
    gpio_set_level(RA8875_RST, 1);
    vTaskDelay(DIV_ROUND_UP(100, portTICK_RATE_MS));

    // Initalize RA8875 clocks (SPI must be decelerated before initializing clocks)
    disp_spi_change_device_speed(SPI_CLOCK_SPEED_SLOW_HZ);
    ra8875_configure_clocks(true);
    disp_spi_change_device_speed(-1);

    // Send all the commands
    for (i = 0; i < INIT_CMDS_SIZE; i++) {
        ra8875_write_cmd(init_cmds[i].cmd, init_cmds[i].data);
    }

	vTaskDelay(DIV_ROUND_UP(10 * 1000, portTICK_RATE_MS));


    // Perform a memory clear (wait maximum of 100 ticks)
    ra8875_write_cmd(RA8875_REG_MCLR, 0x80);
    for(i = 100; i != 0; i--) {
        if ((ra8875_read_cmd(RA8875_REG_MCLR) & 0x80) == 0x00) {
            break;
        }
        vTaskDelay(1);
    }
    if (i == 0) {
        ESP_LOGW(TAG, "WARNING: Memory clear timed out; RA8875 may be unresponsive.");
    }

    // Enable the display and backlight
    ra8875_enable_display(true);
    ra8875_enable_backlight(true);
}

void ra8875_enable_backlight(bool backlight)
{
#if CONFIG_LVGL_ENABLE_BACKLIGHT_CONTROL
    ESP_LOGI(TAG, "%s backlight.", backlight ? "Enabling" : "Disabling");
    uint32_t tmp = 0;

    #if CONFIG_LVGL_BACKLIGHT_ACTIVE_LVL
        tmp = backlight ? 1 : 0;
    #else
        tmp = backlight ? 0 : 1;
    #endif

#ifdef CONFIG_LVGL_DISP_PIN_BCKL
    gpio_set_level(CONFIG_LVGL_DISP_PIN_BCKL, tmp);
#endif

#endif
}

void ra8875_enable_display(bool enable)
{
    ESP_LOGI(TAG, "%s display.", enable ? "Enabling" : "Disabling");
    uint8_t val = enable ? (0x80) : (0x00);
    ra8875_write_cmd(RA8875_REG_PWRR, val);            // Power and Display Control Register (PWRR)
}


void ra8875_flush(lv_disp_drv_t * drv, const lv_area_t * area, lv_color_t * color_map)
{
    static lv_coord_t x1 = LV_COORD_MIN;
    static lv_coord_t x2 = LV_COORD_MIN;
    static lv_coord_t x = LV_COORD_MIN;
    static lv_coord_t y = LV_COORD_MIN;

    size_t linelen = (area->x2 - area->x1 + 1);
    uint8_t * buffer = (uint8_t*)color_map;

#if DEBUG
    ESP_LOGI(TAG, "flush: %d,%d at %d,%d", area->x1, area->x2, area->y1, area->y2 );
#endif

    // Get lock
    disp_spi_acquire();

    // Set window if needed
//    if ((x1 != area->x1) || (x2 != area->x2)) {
//#if DEBUG
//        ESP_LOGI(TAG, "flush: set window (x1,x2): %d,%d -> %d,%d", x1, x2, area->x1, area->x2);
//#endif



        ra8875_set_window(area->x1, area->x2, 0, CONFIG_LVGL_DISPLAY_HEIGHT-1);
        x1 = area->x1;
        x2 = area->x2;
//    }

    // Set cursor if needed
    if ((x != area->x1) || (y != area->y1)) {
//#if DEBUG
        ESP_LOGI(TAG, "flush: set cursor (x,y): %d,%d -> %d,%d", x, y, area->x1, area->y1);
//#endif
        ra8875_set_memory_write_cursor(area->x1, area->y1);
        x = area->x1;
    }

    // Update to future cursor location
    y = area->y2 + 1;
    if (y >= CONFIG_LVGL_DISPLAY_HEIGHT) {
        y = 0;
    }

    // Write data
    ra8875_send_buffer(buffer, (area->y2 - area->y1 + 1)*BYTES_PER_PIXEL*linelen, true);

    // Release lock
    disp_spi_release();
}

void ra8875_sleep_in(void)
{
    disp_spi_change_device_speed(SPI_CLOCK_SPEED_SLOW_HZ);

    ra8875_configure_clocks(false);

    ra8875_write_cmd(RA8875_REG_PWRR, 0x00);           // Power and Display Control Register (PWRR)
    vTaskDelay(DIV_ROUND_UP(20, portTICK_RATE_MS));
    ra8875_write_cmd(RA8875_REG_PWRR, 0x02);           // Power and Display Control Register (PWRR)
}

void ra8875_sleep_out(void)
{
    ra8875_write_cmd(RA8875_REG_PWRR, 0x00);           // Power and Display Control Register (PWRR)
    vTaskDelay(DIV_ROUND_UP(20, portTICK_RATE_MS));

    ra8875_configure_clocks(true);

    disp_spi_change_device_speed(-1);

    ra8875_write_cmd(RA8875_REG_PWRR, 0x80);           // Power and Display Control Register (PWRR)
    vTaskDelay(DIV_ROUND_UP(20, portTICK_RATE_MS));
}

uint8_t ra8875_read_cmd(uint8_t cmd)
{
    uint8_t buf[4] = {RA8875_MODE_CMD_WRITE, cmd, RA8875_MODE_DATA_READ, 0x00};
    disp_spi_transaction(buf, sizeof(buf), (disp_spi_send_flag_t)(DISP_SPI_RECEIVE | DISP_SPI_SEND_POLLING), (disp_spi_read_data*)buf, 0);

	ESP_LOGI(TAG, "writeReg: 0x%02X 0x%02X 0x%02X 0x%02X", buf[0], buf[1], buf[2], buf[3]);

	return buf[3];
}

void ra8875_write_cmd(uint8_t cmd, uint8_t data)
{
    uint8_t buf[4] = {RA8875_MODE_CMD_WRITE, cmd, RA8875_MODE_DATA_WRITE, data};

	ESP_LOGI(TAG, "write_cmd: 0x%02X 0x%02X 0x%02X 0x%02X", buf[0], buf[1], buf[2], buf[3]);

    disp_spi_send_data(buf, sizeof(buf));
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

void ra8875_configure_clocks(bool high_speed)
{
    uint8_t val;

//    val = high_speed ? ((CONFIG_LVGL_DISP_RA8875_PLLDIVM << 7) | CONFIG_LVGL_DISP_RA8875_PLLDIVN) : 0x07;

    val = 0x88;

    ra8875_write_cmd(RA8875_REG_PLLC1, val);           // PLL Control Register 1 (PLLC1)
    vTaskDelay(1);

//    val = high_speed ? CONFIG_LVGL_DISP_RA8875_PLLDIVK : 0x03;
	val = 0x02;

    ra8875_write_cmd(RA8875_REG_PLLC2, val);           // PLL Control Register 2 (PLLC2)
    vTaskDelay(1);

//	ra8875_write_cmd(RA8875_REG_SYSR, 0x0C);
//	vTaskDelay(DIV_ROUND_UP(20, portTICK_RATE_MS));

    ra8875_write_cmd(RA8875_REG_PCSR, PCSR_VAL);            // Pixel Clock Setting Register (PCSR)
    vTaskDelay(DIV_ROUND_UP(20, portTICK_RATE_MS));
}

static void ra8875_set_window(unsigned int xs, unsigned int xe, unsigned int ys, unsigned int ye)
{
//	ys = 0
//	ye = 127
//





//	{RA8875_REG_HSAW0,   0x00},
//	{RA8875_REG_HSAW1,   0x00},
    ra8875_write_cmd(RA8875_REG_HSAW0, (uint8_t)(xs & 0x0FF)); // Horizontal Start Point 0 of Active Window (HSAW0)
    ra8875_write_cmd(RA8875_REG_HSAW1, (uint8_t)(xs >> 8));    // Horizontal Start Point 1 of Active Window (HSAW1)

//	{RA8875_REG_VSAW0,   0x90},
//	{RA8875_REG_VSAW1,   0x90},
    ra8875_write_cmd(RA8875_REG_VSAW0, (uint8_t)((ys + 144) & 0x0FF)); // Vertical Start Point 0 of Active Window (VSAW0)
    ra8875_write_cmd(RA8875_REG_VSAW1, (uint8_t)((ys + 144) >> 8));    // Vertical Start Point 1 of Active Window (VSAW1)

//	{RA8875_REG_HEAW0,   0xDF},
//	{RA8875_REG_HEAW1,   0x01},
    ra8875_write_cmd(RA8875_REG_HEAW0, (uint8_t)(xe & 0x0FF)); // Horizontal End Point 0 of Active Window (HEAW0)
    ra8875_write_cmd(RA8875_REG_HEAW1, (uint8_t)(xe >> 8));    // Horizontal End Point 1 of Active Window (HEAW1)

//	{RA8875_REG_VEAW0,   0x0F},
//	{RA8875_REG_VEAW1,   0x01},
    ra8875_write_cmd(RA8875_REG_VEAW0, (uint8_t)((ye + 144) & 0x0FF)); // Vertical End Point of Active Window 0 (VEAW0)
    ra8875_write_cmd(RA8875_REG_VEAW1, (uint8_t)((ye + 144) >> 8));    // Vertical End Point of Active Window 1 (VEAW1)
}

static void ra8875_set_memory_write_cursor(unsigned int x, unsigned int y)
{
    ra8875_write_cmd(RA8875_REG_CURH0, (uint8_t)(x & 0x0FF));  // Memory Write Cursor Horizontal Position Register 0 (CURH0)
    ra8875_write_cmd(RA8875_REG_CURH1, (uint8_t)(x >> 8));     // Memory Write Cursor Horizontal Position Register 1 (CURH1)
    ra8875_write_cmd(RA8875_REG_CURV0, (uint8_t)((y + 144) & 0x0FF));  // Memory Write Cursor Vertical Position Register 0 (CURV0)
    ra8875_write_cmd(RA8875_REG_CURV1, (uint8_t)((y + 144) >> 8));     // Memory Write Cursor Vertical Position Register 1 (CURV1)
}

static void ra8875_send_buffer(uint8_t * data, size_t length, bool signal_flush)
{
    disp_spi_send_flag_t flags = DISP_SPI_SEND_QUEUED | DISP_SPI_ADDRESS_24;
    if (signal_flush) {
        flags |= DISP_SPI_SIGNAL_FLUSH;
    }
    const uint64_t prefix = (RA8875_MODE_CMD_WRITE << 16)      // Command write mode
                          | (RA8875_REG_MRWC << 8)             // Memory Read/Write Command (MRWC)
                          | (RA8875_MODE_DATA_WRITE);          // Data write mode
    disp_spi_transaction(data, length, flags, NULL, prefix);
}
