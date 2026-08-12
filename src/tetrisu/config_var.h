#ifndef TETRISH_TETRISU_CONFIG_VAR_H
#define TETRISH_TETRISU_CONFIG_VAR_H

struct config_var {
    int port;
    char* address;
    char* ca_path;
};

/*!
    @brief load the tetrisu endpoint and CA certificate configuration

    @pre @p cfg_var is not initialized
    @pre `PROJECT_DIR` names the project directory
    @post on success, @p cfg_var owns NUL-terminated @c address and @c ca_path strings
    @post on failure, @p cfg_var remains uninitialized

    @return `0` on success, `-1` on configuration or allocation failure
*/
int config_var_init(struct config_var* cfg_var);

/*!
    @brief release strings owned by @p cfg
    @pre @p cfg was successfully initialized and has not been freed
    @post @p cfg owns no allocation
*/
void config_var_free(struct config_var* cfg);

#endif
