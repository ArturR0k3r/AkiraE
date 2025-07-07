/**
 * @copyright Copyright © contributors to Project Ocre,
 * which has been established as Project Ocre, a Series of LF Projects, LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OCRE_COMMON_H
#define OCRE_COMMON_H

#include <zephyr/kernel.h>
#include <zephyr/sys/slist.h>
#include <wasm_export.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * CONFIGURATION CONSTANTS
 * ======================================================================== */

#define OCRE_WASM_STACK_SIZE         16384
#define EVENT_THREAD_STACK_SIZE      2048
#define EVENT_THREAD_PRIORITY        5

/* ========================================================================
 * TYPE DEFINITIONS
 * ======================================================================== */

/**
 * @brief Enumeration of OCRE resource types.
 */
typedef enum {
    OCRE_RESOURCE_TYPE_TIMER,  ///< Timer resource
    OCRE_RESOURCE_TYPE_GPIO,   ///< GPIO resource
    OCRE_RESOURCE_TYPE_SENSOR, ///< Sensor resource
    OCRE_RESOURCE_TYPE_COUNT   ///< Total number of resource types (must be last)
} ocre_resource_type_t;

/**
 * @brief Type definition for cleanup handler function.
 *
 * This function is called when a module is being unregistered to clean up
 * any resources associated with that module.
 *
 * @param module_inst The WASM module instance to clean up.
 */
typedef void (*ocre_cleanup_handler_t)(wasm_module_inst_t module_inst);

/**
 * @brief Structure representing the context of an OCRE module.
 */
typedef struct {
    wasm_module_inst_t inst;                                    ///< WASM module instance
    wasm_exec_env_t exec_env;                                   ///< WASM execution environment
    bool in_use;                                                ///< Flag indicating if the module is in use
    uint32_t last_activity;                                     ///< Timestamp of the last activity
    uint32_t resource_count[OCRE_RESOURCE_TYPE_COUNT];          ///< Count of resources per type
    wasm_function_inst_t dispatchers[OCRE_RESOURCE_TYPE_COUNT]; ///< Event dispatchers per resource type
} ocre_module_context_t;

/**
 * @brief Structure representing a WASM event for internal processing.
 */
typedef struct {
    uint32_t type;  ///< Event type (ocre_resource_type_t)
    uint32_t id;    ///< Event ID (resource-specific identifier)
    uint32_t port;  ///< Port/channel associated with the event
    uint32_t state; ///< State/value associated with the event
} wasm_event_t;

/**
 * @brief Structure representing an OCRE event for external posting.
 */
typedef struct {
    union {
        struct {
            uint32_t timer_id;        ///< Timer ID
            wasm_module_inst_t owner; ///< Owner module instance
        } timer_event;                ///< Timer event data
        
        struct {
            uint32_t pin_id;          ///< GPIO pin ID
            uint32_t state;           ///< GPIO state (0 or 1)
            wasm_module_inst_t owner; ///< Owner module instance
        } gpio_event;                 ///< GPIO event data
        
        struct {
            uint32_t sensor_id;       ///< Sensor ID
            uint32_t channel;         ///< Sensor channel
            uint32_t value;           ///< Sensor value
            wasm_module_inst_t owner; ///< Owner module instance
        } sensor_event;               ///< Sensor event data
    } data;                           ///< Union of event data
    
    ocre_resource_type_t type;        ///< Type of the event
} ocre_event_t;

/* ========================================================================
 * GLOBAL VARIABLES
 * ======================================================================== */

/**
 * @brief Global flag indicating if the common system is initialized.
 */
extern bool common_initialized;

/**
 * @brief Thread-local storage for the current WASM module instance.
 */
extern __thread wasm_module_inst_t *current_module_tls;

/* ========================================================================
 * CORE SYSTEM FUNCTIONS
 * ======================================================================== */

/**
 * @brief Initialize the OCRE common system.
 *
 * This function initializes all the common components required for OCRE operations,
 * including the module registry, event processing system, and worker threads.
 *
 * @return 0 on success, negative error code on failure:
 *         - -ENOMEM: Out of memory
 *         - -EBUSY: System already initialized
 */
int ocre_common_init(void);

/**
 * @brief Shutdown the OCRE common system.
 *
 * This function gracefully shuts down the OCRE common system, stopping all
 * worker threads and cleaning up all registered modules.
 */
void ocre_common_shutdown(void);

/* ========================================================================
 * MODULE MANAGEMENT
 * ======================================================================== */

/**
 * @brief Register a WASM module with the OCRE system.
 *
 * This function registers a WASM module instance with the OCRE system, creating
 * the necessary execution environment and context for the module.
 *
 * @param module_inst The WASM module instance to register.
 * @return 0 on success, negative error code on failure:
 *         - -EINVAL: Invalid module instance
 *         - -ENOMEM: Out of memory
 *         - -ENODEV: Common system not initialized
 */
int ocre_register_module(wasm_module_inst_t module_inst);

/**
 * @brief Unregister a WASM module from the OCRE system.
 *
 * This function unregisters a WASM module instance from the OCRE system,
 * cleaning up all associated resources and destroying the execution environment.
 *
 * @param module_inst The WASM module instance to unregister.
 */
void ocre_unregister_module(wasm_module_inst_t module_inst);

/**
 * @brief Get the context of a registered WASM module.
 *
 * This function retrieves the context associated with a registered WASM module
 * instance. The context contains execution environment, resource counts, and
 * other module-specific information.
 *
 * @param module_inst The WASM module instance.
 * @return Pointer to the module context, or NULL if not found.
 */
ocre_module_context_t *ocre_get_module_context(wasm_module_inst_t module_inst);

/**
 * @brief Get the current WASM module instance.
 *
 * This function returns the currently active WASM module instance from
 * thread-local storage.
 *
 * @return The current WASM module instance, or NULL if not set.
 */
wasm_module_inst_t ocre_get_current_module(void);

/* ========================================================================
 * EVENT SYSTEM
 * ======================================================================== */

/**
 * @brief Register an event dispatcher for a specific resource type.
 *
 * This function registers a WASM function as an event dispatcher for a specific
 * resource type. When events of that type occur, the dispatcher function will
 * be called with the event data.
 *
 * @param exec_env WASM execution environment.
 * @param type Resource type to register the dispatcher for.
 * @param function_name Name of the WASM function to use as dispatcher.
 * @return 0 on success, negative error code on failure:
 *         - -EINVAL: Invalid parameters
 *         - -ENOENT: Function not found or module context not found
 */
int ocre_register_dispatcher(wasm_exec_env_t exec_env, ocre_resource_type_t type, 
                           const char *function_name);

/**
 * @brief Post an event to the OCRE event system.
 *
 * This function posts an event to the OCRE event processing system. The event
 * will be queued and processed asynchronously by the event processing threads.
 *
 * @param event Pointer to the event to post.
 * @return 0 on success, negative error code on failure:
 *         - -EINVAL: Invalid event or event type
 *         - -ENOMEM: Event queue full
 *         - -ENODEV: Common system not initialized
 *         - -EIO: Failed to post event
 */
int ocre_post_event(ocre_event_t *event);

/**
 * @brief Get an event from the WASM event queue.
 *
 * This function allows WASM modules to retrieve events from the event queue.
 * The event data is copied to the provided WASM memory offsets.
 *
 * @param exec_env WASM execution environment.
 * @param type_offset Offset in WASM memory for event type.
 * @param id_offset Offset in WASM memory for event ID.
 * @param port_offset Offset in WASM memory for event port.
 * @param state_offset Offset in WASM memory for event state.
 * @return 0 on success, negative error code on failure:
 *         - -EINVAL: Invalid parameters or memory offsets
 *         - -ENOENT: No events available
 */
int ocre_get_event(wasm_exec_env_t exec_env, uint32_t type_offset, uint32_t id_offset, 
                  uint32_t port_offset, uint32_t state_offset);

/* ========================================================================
 * RESOURCE MANAGEMENT
 * ======================================================================== */

/**
 * @brief Get the count of resources of a specific type for a module.
 *
 * This function returns the number of resources of the specified type that
 * are currently allocated to the given module.
 *
 * @param module_inst The WASM module instance.
 * @param type Resource type to query.
 * @return Number of resources, or 0 if module not found or invalid type.
 */
uint32_t ocre_get_resource_count(wasm_module_inst_t module_inst, ocre_resource_type_t type);

/**
 * @brief Increment the resource count for a specific type.
 *
 * This function increments the resource count for a specific resource type
 * associated with a module. This is typically called when a resource is
 * allocated to a module.
 *
 * @param module_inst The WASM module instance.
 * @param type Resource type to increment.
 */
void ocre_increment_resource_count(wasm_module_inst_t module_inst, ocre_resource_type_t type);

/**
 * @brief Decrement the resource count for a specific type.
 *
 * This function decrements the resource count for a specific resource type
 * associated with a module. This is typically called when a resource is
 * freed from a module.
 *
 * @param module_inst The WASM module instance.
 * @param type Resource type to decrement.
 */
void ocre_decrement_resource_count(wasm_module_inst_t module_inst, ocre_resource_type_t type);

/**
 * @brief Register a cleanup handler for a resource type.
 *
 * This function registers a cleanup handler function that will be called when
 * a module is being unregistered. The handler should clean up any resources
 * of the specified type that are associated with the module.
 *
 * @param type Resource type to register the cleanup handler for.
 * @param handler Cleanup handler function.
 * @return 0 on success, negative error code on failure:
 *         - -EINVAL: Invalid parameters
 */
int ocre_register_cleanup_handler(ocre_resource_type_t type, ocre_cleanup_handler_t handler);

/**
 * @brief Clean up resources for a module.
 *
 * This function calls all registered cleanup handlers for the specified module.
 * It is typically called when a module is being unregistered.
 *
 * @param module_inst The WASM module instance to clean up resources for.
 */
void ocre_cleanup_module_resources(wasm_module_inst_t module_inst);

/* ========================================================================
 * UTILITY MACROS
 * ======================================================================== */

/**
 * @brief Check if a resource type is valid.
 */
#define OCRE_IS_VALID_RESOURCE_TYPE(type) ((type) < OCRE_RESOURCE_TYPE_COUNT)

/**
 * @brief Check if a module instance is valid.
 */
#define OCRE_IS_VALID_MODULE(module) ((module) != NULL)

#ifdef __cplusplus
}
#endif

#endif /* OCRE_COMMON_H */