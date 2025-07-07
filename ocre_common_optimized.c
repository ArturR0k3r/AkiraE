/**
 * @copyright Copyright © contributors to Project Ocre,
 * which has been established as Project Ocre, a Series of LF Projects, LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <ocre/ocre.h>
#include "ocre_core_external.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/mem_manage.h>
#include <zephyr/drivers/mm/system_mm.h>
#include <zephyr/sys/slist.h>
#include <zephyr/sys/ring_buffer.h>
#include <stdlib.h>
#include <autoconf.h>
#include "../../../../../wasm-micro-runtime/core/iwasm/include/lib_export.h"
#include "bh_log.h"
#include "../ocre_timers/ocre_timer.h"
#include "../ocre_gpio/ocre_gpio.h"
#include "ocre_common.h"

LOG_MODULE_DECLARE(ocre_cs_component, OCRE_LOG_LEVEL);

/* ========================================================================
 * CONSTANTS AND CONFIGURATION
 * ======================================================================== */

#define EVENT_BUFFER_SIZE           1024
#define EVENT_THREAD_POOL_SIZE      2
#define EVENT_BATCH_SIZE            16
#define MAX_DISPATCH_RETRIES        3
#define CLEANUP_TIMEOUT_MS          1000

/* ========================================================================
 * GLOBAL STATE AND STRUCTURES
 * ======================================================================== */

// Unified event buffer using ring buffer for better performance
static uint8_t event_buffer[EVENT_BUFFER_SIZE];
static struct ring_buf event_ring;
static struct k_sem event_sem;
static struct k_mutex event_mutex;

// Module registry
typedef struct module_node {
    ocre_module_context_t ctx;
    sys_snode_t node;
    struct k_mutex ctx_mutex;  // Per-module mutex for finer granularity
} module_node_t;

static sys_slist_t module_registry;
static struct k_mutex registry_mutex;

// Cleanup handlers
static struct {
    ocre_resource_type_t type;
    ocre_cleanup_handler_t handler;
} cleanup_handlers[OCRE_RESOURCE_TYPE_COUNT];

// Event processing threads
static struct core_thread event_threads[EVENT_THREAD_POOL_SIZE];
static volatile bool event_system_running = false;

// Thread-local storage for current module
__thread wasm_module_inst_t *current_module_tls = NULL;

// System state
static bool common_initialized = false;

/* ========================================================================
 * UTILITY FUNCTIONS
 * ======================================================================== */

/**
 * @brief Validate event parameters
 */
static inline bool is_valid_event_type(ocre_resource_type_t type) {
    return type < OCRE_RESOURCE_TYPE_COUNT;
}

/**
 * @brief Validate module instance
 */
static inline bool is_valid_module_instance(wasm_module_inst_t module_inst) {
    return module_inst != NULL;
}

/**
 * @brief Log event processing for debugging
 */
static void log_event_processing(const wasm_event_t *event, const char *action) {
    LOG_DBG("Event %s: type=%d, id=%d, port=%d, state=%d", 
            action, event->type, event->id, event->port, event->state);
}

/* ========================================================================
 * EVENT PROCESSING CORE
 * ======================================================================== */

/**
 * @brief Process a single event with improved error handling
 */
static int process_single_event(const wasm_event_t *event, module_node_t *node) {
    if (!event || !node) {
        return -EINVAL;
    }

    if (!is_valid_event_type(event->type)) {
        LOG_ERR("Invalid event type: %d", event->type);
        return -EINVAL;
    }

    wasm_function_inst_t dispatcher = node->ctx.dispatchers[event->type];
    if (!dispatcher) {
        LOG_WRN("No dispatcher for event type %d in module %p", 
                event->type, (void *)node->ctx.inst);
        return -ENOENT;
    }

    if (!node->ctx.exec_env) {
        LOG_ERR("Null exec_env for module %p", (void *)node->ctx.inst);
        return -EINVAL;
    }

    // Prepare arguments based on event type
    uint32_t args[3] = {0};
    uint32_t arg_count = 0;
    
    switch (event->type) {
        case OCRE_RESOURCE_TYPE_TIMER:
            args[0] = event->id;
            arg_count = 1;
            break;
        case OCRE_RESOURCE_TYPE_GPIO:
            args[0] = event->id;
            args[1] = event->state;
            arg_count = 2;
            break;
        case OCRE_RESOURCE_TYPE_SENSOR:
            args[0] = event->id;
            args[1] = event->port;
            args[2] = event->state;
            arg_count = 3;
            break;
        default:
            LOG_ERR("Unhandled event type: %d", event->type);
            return -EINVAL;
    }

    // Set current module context
    current_module_tls = &node->ctx.inst;
    
    // Execute WASM function with retry logic
    bool result = false;
    int retry_count = 0;
    
    while (!result && retry_count < MAX_DISPATCH_RETRIES) {
        result = wasm_runtime_call_wasm(node->ctx.exec_env, dispatcher, arg_count, args);
        
        if (!result) {
            const char *exception = wasm_runtime_get_exception(node->ctx.inst);
            LOG_WRN("WASM call failed (attempt %d): %s", retry_count + 1, exception);
            
            // Clear exception for retry
            wasm_runtime_clear_exception(node->ctx.inst);
            retry_count++;
            
            // Brief delay before retry
            k_sleep(K_MSEC(1));
        }
    }

    // Clear current module context
    current_module_tls = NULL;
    
    if (result) {
        // Update activity timestamp
        k_mutex_lock(&node->ctx_mutex, K_FOREVER);
        node->ctx.last_activity = k_uptime_get_32();
        k_mutex_unlock(&node->ctx_mutex);
        
        log_event_processing(event, "processed");
        return 0;
    } else {
        LOG_ERR("Event processing failed after %d retries", MAX_DISPATCH_RETRIES);
        return -EFAULT;
    }
}

/**
 * @brief Find module node by instance with improved lookup
 */
static module_node_t *find_module_node(wasm_module_inst_t module_inst) {
    module_node_t *node;
    
    SYS_SLIST_FOR_EACH_CONTAINER(&module_registry, node, node) {
        if (node->ctx.inst == module_inst) {
            return node;
        }
    }
    
    return NULL;
}

/**
 * @brief Enhanced event processing thread
 */
static void event_processor_thread(void *arg1, void *arg2, void *arg3) {
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    
    int thread_id = (int)(uintptr_t)arg1;
    
    LOG_INF("Event processor thread %d started", thread_id);
    
    // Initialize WASM runtime for this thread
    if (!wasm_runtime_init_thread_env()) {
        LOG_ERR("Failed to initialize WASM runtime for thread %d", thread_id);
        return;
    }

    wasm_event_t event_batch[EVENT_BATCH_SIZE];
    
    while (event_system_running) {
        // Wait for events to be available
        int ret = k_sem_take(&event_sem, K_FOREVER);
        if (ret != 0 || !event_system_running) {
            break;
        }

        // Process events in batches for better performance
        k_mutex_lock(&event_mutex, K_FOREVER);
        
        uint32_t bytes_read = ring_buf_get(&event_ring, (uint8_t *)event_batch, 
                                         sizeof(event_batch));
        uint32_t events_count = bytes_read / sizeof(wasm_event_t);
        
        k_mutex_unlock(&event_mutex);
        
        // Process each event in the batch
        for (uint32_t i = 0; i < events_count; i++) {
            wasm_event_t *event = &event_batch[i];
            
            log_event_processing(event, "processing");
            
            // Find target module
            k_mutex_lock(&registry_mutex, K_FOREVER);
            module_node_t *target_node = find_module_node(
                current_module_tls ? *current_module_tls : NULL);
            k_mutex_unlock(&registry_mutex);
            
            if (!target_node) {
                LOG_WRN("No target module found for event type %d", event->type);
                continue;
            }
            
            // Process the event
            int result = process_single_event(event, target_node);
            if (result != 0) {
                LOG_ERR("Failed to process event type %d: %d", event->type, result);
            }
        }
    }
    
    // Clean up WASM runtime
    wasm_runtime_destroy_thread_env();
    LOG_INF("Event processor thread %d terminated", thread_id);
}

/* ========================================================================
 * PUBLIC API IMPLEMENTATION
 * ======================================================================== */

int ocre_common_init(void) {
    if (common_initialized) {
        LOG_INF("OCRE common already initialized");
        return 0;
    }

    // Initialize synchronization primitives
    k_mutex_init(&registry_mutex);
    k_mutex_init(&event_mutex);
    k_sem_init(&event_sem, 0, UINT_MAX);
    
    // Initialize data structures
    sys_slist_init(&module_registry);
    ring_buf_init(&event_ring, EVENT_BUFFER_SIZE, event_buffer);
    
    // Clear cleanup handlers
    memset(cleanup_handlers, 0, sizeof(cleanup_handlers));
    
    // Start event processing system
    event_system_running = true;
    
    // Create event processing threads
    for (int i = 0; i < EVENT_THREAD_POOL_SIZE; i++) {
        char thread_name[32];
        snprintf(thread_name, sizeof(thread_name), "ocre_event_%d", i);
        
        int ret = core_thread_create(&event_threads[i], 
                                   event_processor_thread, 
                                   (void *)(uintptr_t)i, 
                                   thread_name, 
                                   EVENT_THREAD_STACK_SIZE, 
                                   EVENT_THREAD_PRIORITY);
        if (ret != 0) {
            LOG_ERR("Failed to create event thread %d: %d", i, ret);
            ocre_common_shutdown();
            return ret;
        }
    }
    
    common_initialized = true;
    LOG_INF("OCRE common initialized successfully");
    return 0;
}

void ocre_common_shutdown(void) {
    if (!common_initialized) {
        return;
    }
    
    LOG_INF("Shutting down OCRE common system");
    
    // Signal threads to stop
    event_system_running = false;
    
    // Wake up all waiting threads
    for (int i = 0; i < EVENT_THREAD_POOL_SIZE; i++) {
        k_sem_give(&event_sem);
    }
    
    // Wait for threads to terminate (with timeout)
    for (int i = 0; i < EVENT_THREAD_POOL_SIZE; i++) {
        // Note: core_thread_join would be ideal here if available
        k_sleep(K_MSEC(100));
    }
    
    // Clean up all registered modules
    k_mutex_lock(&registry_mutex, K_FOREVER);
    module_node_t *node, *tmp;
    SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&module_registry, node, tmp, node) {
        ocre_cleanup_module_resources(node->ctx.inst);
        if (node->ctx.exec_env) {
            wasm_runtime_destroy_exec_env(node->ctx.exec_env);
        }
        sys_slist_remove(&module_registry, NULL, &node->node);
        k_free(node);
    }
    k_mutex_unlock(&registry_mutex);
    
    common_initialized = false;
    LOG_INF("OCRE common shutdown complete");
}

int ocre_register_module(wasm_module_inst_t module_inst) {
    if (!is_valid_module_instance(module_inst)) {
        LOG_ERR("Invalid module instance");
        return -EINVAL;
    }
    
    if (!common_initialized) {
        LOG_ERR("OCRE common not initialized");
        return -ENODEV;
    }
    
    // Allocate module node
    module_node_t *node = k_malloc(sizeof(module_node_t));
    if (!node) {
        LOG_ERR("Failed to allocate module node");
        return -ENOMEM;
    }
    
    // Initialize module context
    memset(&node->ctx, 0, sizeof(node->ctx));
    node->ctx.inst = module_inst;
    node->ctx.in_use = true;
    node->ctx.last_activity = k_uptime_get_32();
    
    // Initialize per-module mutex
    k_mutex_init(&node->ctx_mutex);
    
    // Create execution environment
    node->ctx.exec_env = wasm_runtime_create_exec_env(module_inst, OCRE_WASM_STACK_SIZE);
    if (!node->ctx.exec_env) {
        LOG_ERR("Failed to create exec env for module %p", (void *)module_inst);
        k_free(node);
        return -ENOMEM;
    }
    
    // Add to registry
    k_mutex_lock(&registry_mutex, K_FOREVER);
    sys_slist_append(&module_registry, &node->node);
    k_mutex_unlock(&registry_mutex);
    
    LOG_INF("Module registered: %p", (void *)module_inst);
    return 0;
}

void ocre_unregister_module(wasm_module_inst_t module_inst) {
    if (!is_valid_module_instance(module_inst)) {
        LOG_ERR("Invalid module instance");
        return;
    }
    
    k_mutex_lock(&registry_mutex, K_FOREVER);
    
    module_node_t *node = find_module_node(module_inst);
    if (node) {
        // Clean up resources
        ocre_cleanup_module_resources(module_inst);
        
        // Destroy execution environment
        if (node->ctx.exec_env) {
            wasm_runtime_destroy_exec_env(node->ctx.exec_env);
        }
        
        // Remove from registry
        sys_slist_remove(&module_registry, NULL, &node->node);
        k_free(node);
        
        LOG_INF("Module unregistered: %p", (void *)module_inst);
    } else {
        LOG_WRN("Module not found in registry: %p", (void *)module_inst);
    }
    
    k_mutex_unlock(&registry_mutex);
}

ocre_module_context_t *ocre_get_module_context(wasm_module_inst_t module_inst) {
    if (!is_valid_module_instance(module_inst)) {
        return NULL;
    }
    
    k_mutex_lock(&registry_mutex, K_FOREVER);
    module_node_t *node = find_module_node(module_inst);
    k_mutex_unlock(&registry_mutex);
    
    if (node) {
        k_mutex_lock(&node->ctx_mutex, K_FOREVER);
        node->ctx.last_activity = k_uptime_get_32();
        k_mutex_unlock(&node->ctx_mutex);
        return &node->ctx;
    }
    
    return NULL;
}

int ocre_register_dispatcher(wasm_exec_env_t exec_env, ocre_resource_type_t type, 
                           const char *function_name) {
    if (!exec_env || !function_name || !is_valid_event_type(type)) {
        LOG_ERR("Invalid dispatcher parameters");
        return -EINVAL;
    }
    
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!is_valid_module_instance(module_inst)) {
        LOG_ERR("No module instance available");
        return -EINVAL;
    }
    
    // Look up the function
    wasm_function_inst_t func = wasm_runtime_lookup_function(module_inst, function_name);
    if (!func) {
        LOG_ERR("Function '%s' not found in module %p", function_name, (void *)module_inst);
        return -ENOENT;
    }
    
    // Get module context
    ocre_module_context_t *ctx = ocre_get_module_context(module_inst);
    if (!ctx) {
        LOG_ERR("Module context not found for %p", (void *)module_inst);
        return -ENOENT;
    }
    
    // Register the dispatcher
    k_mutex_lock(&registry_mutex, K_FOREVER);
    ctx->dispatchers[type] = func;
    k_mutex_unlock(&registry_mutex);
    
    LOG_INF("Registered dispatcher for type %d: %s", type, function_name);
    return 0;
}

int ocre_post_event(ocre_event_t *event) {
    if (!event || !is_valid_event_type(event->type)) {
        LOG_ERR("Invalid event or event type");
        return -EINVAL;
    }
    
    if (!common_initialized) {
        LOG_ERR("OCRE common not initialized");
        return -ENODEV;
    }
    
    // Convert ocre_event_t to wasm_event_t
    wasm_event_t wasm_event = {0};
    wasm_event.type = event->type;
    
    switch (event->type) {
        case OCRE_RESOURCE_TYPE_TIMER:
            wasm_event.id = event->data.timer_event.timer_id;
            break;
        case OCRE_RESOURCE_TYPE_GPIO:
            wasm_event.id = event->data.gpio_event.pin_id;
            wasm_event.state = event->data.gpio_event.state;
            break;
        case OCRE_RESOURCE_TYPE_SENSOR:
            wasm_event.id = event->data.sensor_event.sensor_id;
            wasm_event.port = event->data.sensor_event.channel;
            wasm_event.state = event->data.sensor_event.value;
            break;
        default:
            LOG_ERR("Unhandled event type: %d", event->type);
            return -EINVAL;
    }
    
    // Post to ring buffer
    k_mutex_lock(&event_mutex, K_FOREVER);
    
    if (ring_buf_space_get(&event_ring) < sizeof(wasm_event_t)) {
        k_mutex_unlock(&event_mutex);
        LOG_ERR("Event buffer full");
        return -ENOMEM;
    }
    
    uint32_t bytes_written = ring_buf_put(&event_ring, (uint8_t *)&wasm_event, 
                                         sizeof(wasm_event_t));
    k_mutex_unlock(&event_mutex);
    
    if (bytes_written != sizeof(wasm_event_t)) {
        LOG_ERR("Failed to post event to buffer");
        return -EIO;
    }
    
    // Signal event processors
    k_sem_give(&event_sem);
    
    LOG_DBG("Posted event: type=%d", event->type);
    return 0;
}

int ocre_get_event(wasm_exec_env_t exec_env, uint32_t type_offset, uint32_t id_offset,
                  uint32_t port_offset, uint32_t state_offset) {
    if (!exec_env) {
        LOG_ERR("Invalid exec_env");
        return -EINVAL;
    }
    
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!is_valid_module_instance(module_inst)) {
        LOG_ERR("No module instance available");
        return -EINVAL;
    }
    
    // Convert offsets to native addresses
    int32_t *type_native = (int32_t *)wasm_runtime_addr_app_to_native(module_inst, type_offset);
    int32_t *id_native = (int32_t *)wasm_runtime_addr_app_to_native(module_inst, id_offset);
    int32_t *port_native = (int32_t *)wasm_runtime_addr_app_to_native(module_inst, port_offset);
    int32_t *state_native = (int32_t *)wasm_runtime_addr_app_to_native(module_inst, state_offset);
    
    if (!type_native || !id_native || !port_native || !state_native) {
        LOG_ERR("Invalid memory offsets");
        return -EINVAL;
    }
    
    // Try to get an event from the ring buffer
    wasm_event_t event;
    k_mutex_lock(&event_mutex, K_FOREVER);
    
    uint32_t bytes_read = ring_buf_get(&event_ring, (uint8_t *)&event, sizeof(event));
    k_mutex_unlock(&event_mutex);
    
    if (bytes_read != sizeof(event)) {
        return -ENOENT;  // No events available
    }
    
    // Copy event data to WASM memory
    *type_native = event.type;
    *id_native = event.id;
    *port_native = event.port;
    *state_native = event.state;
    
    LOG_DBG("Retrieved event: type=%d, id=%d, port=%d, state=%d", 
            event.type, event.id, event.port, event.state);
    
    return 0;
}

/* ========================================================================
 * RESOURCE MANAGEMENT
 * ======================================================================== */

uint32_t ocre_get_resource_count(wasm_module_inst_t module_inst, ocre_resource_type_t type) {
    if (!is_valid_module_instance(module_inst) || !is_valid_event_type(type)) {
        return 0;
    }
    
    ocre_module_context_t *ctx = ocre_get_module_context(module_inst);
    return ctx ? ctx->resource_count[type] : 0;
}

void ocre_increment_resource_count(wasm_module_inst_t module_inst, ocre_resource_type_t type) {
    if (!is_valid_module_instance(module_inst) || !is_valid_event_type(type)) {
        return;
    }
    
    k_mutex_lock(&registry_mutex, K_FOREVER);
    module_node_t *node = find_module_node(module_inst);
    if (node) {
        k_mutex_lock(&node->ctx_mutex, K_FOREVER);
        node->ctx.resource_count[type]++;
        LOG_DBG("Incremented resource count: type=%d, count=%d", 
                type, node->ctx.resource_count[type]);
        k_mutex_unlock(&node->ctx_mutex);
    }
    k_mutex_unlock(&registry_mutex);
}

void ocre_decrement_resource_count(wasm_module_inst_t module_inst, ocre_resource_type_t type) {
    if (!is_valid_module_instance(module_inst) || !is_valid_event_type(type)) {
        return;
    }
    
    k_mutex_lock(&registry_mutex, K_FOREVER);
    module_node_t *node = find_module_node(module_inst);
    if (node) {
        k_mutex_lock(&node->ctx_mutex, K_FOREVER);
        if (node->ctx.resource_count[type] > 0) {
            node->ctx.resource_count[type]--;
            LOG_DBG("Decremented resource count: type=%d, count=%d", 
                    type, node->ctx.resource_count[type]);
        }
        k_mutex_unlock(&node->ctx_mutex);
    }
    k_mutex_unlock(&registry_mutex);
}

int ocre_register_cleanup_handler(ocre_resource_type_t type, ocre_cleanup_handler_t handler) {
    if (!is_valid_event_type(type) || !handler) {
        LOG_ERR("Invalid cleanup handler parameters");
        return -EINVAL;
    }
    
    cleanup_handlers[type].type = type;
    cleanup_handlers[type].handler = handler;
    
    LOG_INF("Registered cleanup handler for type %d", type);
    return 0;
}

void ocre_cleanup_module_resources(wasm_module_inst_t module_inst) {
    if (!is_valid_module_instance(module_inst)) {
        return;
    }
    
    LOG_INF("Cleaning up resources for module %p", (void *)module_inst);
    
    for (int i = 0; i < OCRE_RESOURCE_TYPE_COUNT; i++) {
        if (cleanup_handlers[i].handler) {
            cleanup_handlers[i].handler(module_inst);
        }
    }
}

wasm_module_inst_t ocre_get_current_module(void) {
    return current_module_tls ? *current_module_tls : NULL;
}