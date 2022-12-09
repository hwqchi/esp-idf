/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/cdefs.h>
#include "freertos/FreeRTOS.h"
#include "esp_check.h"
#include "glitch_filter_priv.h"
#include "esp_private/esp_clk.h"
#include "soc/soc_caps.h"
#include "hal/gpio_glitch_filter_ll.h"

static const char *TAG = "gpio-filter";

typedef struct gpio_flex_glitch_filter_t gpio_flex_glitch_filter_t;

typedef struct gpio_flex_glitch_filter_group_t {
    gpio_glitch_filter_dev_t *hw;
    gpio_flex_glitch_filter_t *filters[SOC_GPIO_FLEX_GLITCH_FILTER_NUM];
    portMUX_TYPE spinlock;
} gpio_flex_glitch_filter_group_t;

struct gpio_flex_glitch_filter_t {
    gpio_glitch_filter_t base;
    gpio_flex_glitch_filter_group_t *group;
    uint32_t filter_id;
};

static gpio_flex_glitch_filter_group_t s_gpio_glitch_filter_group = {
    .hw = &GLITCH_FILTER,
    .spinlock = portMUX_INITIALIZER_UNLOCKED,
};

static esp_err_t gpio_filter_register_to_group(gpio_flex_glitch_filter_t *filter)
{
    gpio_flex_glitch_filter_group_t *group = &s_gpio_glitch_filter_group;
    int filter_id = -1;
    // loop to search free one in the group
    portENTER_CRITICAL(&group->spinlock);
    for (int j = 0; j < SOC_GPIO_FLEX_GLITCH_FILTER_NUM; j++) {
        if (!group->filters[j]) {
            filter_id = j;
            group->filters[j] = filter;
            break;
        }
    }
    portEXIT_CRITICAL(&group->spinlock);

    ESP_RETURN_ON_FALSE(filter_id != -1, ESP_ERR_NOT_FOUND, TAG, "no free gpio glitch filter");
    filter->filter_id = filter_id;
    filter->group = group;
    return ESP_OK;
}

static esp_err_t gpio_filter_destroy(gpio_flex_glitch_filter_t *filter)
{
    gpio_flex_glitch_filter_group_t *group = &s_gpio_glitch_filter_group;
    int filter_id = filter->filter_id;

    // unregister the filter from the group
    if (filter->group) {
        portENTER_CRITICAL(&group->spinlock);
        group->filters[filter_id] = NULL;
        portEXIT_CRITICAL(&group->spinlock);
    }

    free(filter);
    return ESP_OK;
}

static esp_err_t gpio_flex_glitch_filter_del(gpio_glitch_filter_t *filter)
{
    ESP_RETURN_ON_FALSE(filter->fsm == GLITCH_FILTER_FSM_INIT, ESP_ERR_INVALID_STATE, TAG, "filter not in init state");
    gpio_flex_glitch_filter_t *flex_filter = __containerof(filter, gpio_flex_glitch_filter_t, base);
    return gpio_filter_destroy(flex_filter);
}

static esp_err_t gpio_flex_glitch_filter_enable(gpio_glitch_filter_t *filter)
{
    ESP_RETURN_ON_FALSE(filter->fsm == GLITCH_FILTER_FSM_INIT, ESP_ERR_INVALID_STATE, TAG, "filter not in init state");
    gpio_flex_glitch_filter_t *flex_filter = __containerof(filter, gpio_flex_glitch_filter_t, base);

    int filter_id = flex_filter->filter_id;
    gpio_ll_glitch_filter_enable(s_gpio_glitch_filter_group.hw, filter_id, true);
    filter->fsm = GLITCH_FILTER_FSM_ENABLE;
    return ESP_OK;
}

static esp_err_t gpio_flex_glitch_filter_disable(gpio_glitch_filter_t *filter)
{
    ESP_RETURN_ON_FALSE(filter->fsm == GLITCH_FILTER_FSM_ENABLE, ESP_ERR_INVALID_STATE, TAG, "filter not in enable state");
    gpio_flex_glitch_filter_t *flex_filter = __containerof(filter, gpio_flex_glitch_filter_t, base);

    int filter_id = flex_filter->filter_id;
    gpio_ll_glitch_filter_enable(s_gpio_glitch_filter_group.hw, filter_id, false);
    filter->fsm = GLITCH_FILTER_FSM_INIT;
    return ESP_OK;
}

esp_err_t gpio_new_flex_glitch_filter(const gpio_flex_glitch_filter_config_t *config, gpio_glitch_filter_handle_t *ret_filter)
{
    esp_err_t ret = ESP_OK;
    gpio_flex_glitch_filter_t *filter = NULL;
    ESP_GOTO_ON_FALSE(config && ret_filter, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    ESP_GOTO_ON_FALSE(GPIO_IS_VALID_GPIO(config->gpio_num), ESP_ERR_INVALID_ARG, err, TAG, "invalid gpio number");
    // Glitch filter's clock source is same to the IOMUX clock
    // TODO: IDF-6345 task will make the IOMUX clock source configurable, and we should opt the glitch filter clock source accordingly
    uint32_t clk_freq_mhz = esp_clk_xtal_freq() / 1000000;
    uint32_t window_thres_ticks = clk_freq_mhz * config->window_thres_ns / 1000;
    uint32_t window_width_ticks = clk_freq_mhz * config->window_width_ns / 1000;
    ESP_GOTO_ON_FALSE(window_thres_ticks && window_thres_ticks <= window_width_ticks && window_width_ticks <= GPIO_LL_GLITCH_FILTER_MAX_WINDOW,
                      ESP_ERR_INVALID_ARG, err, TAG, "invalid or out of range window width/threshold");

    filter = heap_caps_calloc(1, sizeof(gpio_flex_glitch_filter_t), FILTER_MEM_ALLOC_CAPS);
    ESP_GOTO_ON_FALSE(filter, ESP_ERR_NO_MEM, err, TAG, "no memory for flex glitch filter");
    // register the filter to the group
    ESP_GOTO_ON_ERROR(gpio_filter_register_to_group(filter), err, TAG, "register filter to group failed");
    int filter_id = filter->filter_id;

    // make sure the filter is disabled
    gpio_ll_glitch_filter_enable(s_gpio_glitch_filter_group.hw, filter_id, false);
    // apply the filter to the GPIO
    gpio_ll_glitch_filter_set_gpio(s_gpio_glitch_filter_group.hw, filter_id, config->gpio_num);
    // set filter coefficient
    gpio_ll_glitch_filter_set_window_coeff(s_gpio_glitch_filter_group.hw, filter_id, window_width_ticks, window_thres_ticks);

    filter->base.gpio_num = config->gpio_num;
    filter->base.fsm = GLITCH_FILTER_FSM_INIT;
    filter->base.del = gpio_flex_glitch_filter_del;
    filter->base.enable = gpio_flex_glitch_filter_enable;
    filter->base.disable = gpio_flex_glitch_filter_disable;

    *ret_filter = &(filter->base);
    return ESP_OK;
err:
    if (filter) {
        gpio_filter_destroy(filter);
    }
    return ret;
}
