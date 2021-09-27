#pragma once

#include <assert.h>
#include <dlfcn.h>

/* Wasm initialization functions generated by the compiler */
#define AWSM_ABI_INITIALIZE_GLOBALS "populate_globals"
#define AWSM_ABI_INITIALIZE_MEMORY  "populate_memory"
#define AWSM_ABI_INITIALIZE_TABLE   "populate_table"
#define AWSM_ABI_INITIALIZE_LIBC    "wasmf___init_libc"
#define AWSM_ABI_ENTRYPOINT         "wasmf_main"

/* functions in the module to lookup and call per sandbox. */
typedef int32_t (*awsm_abi_entrypoint_fn_t)(int32_t a, int32_t b);
typedef void (*awsm_abi_init_globals_fn_t)(void);
typedef void (*awsm_abi_init_mem_fn_t)(void);
typedef void (*awsm_abi_init_tbl_fn_t)(void);
typedef void (*awsm_abi_init_libc_fn_t)(int32_t, int32_t);

struct awsm_abi {
	void *                     handle;
	awsm_abi_init_globals_fn_t initialize_globals;
	awsm_abi_init_mem_fn_t     initialize_memory;
	awsm_abi_init_tbl_fn_t     initialize_tables;
	awsm_abi_init_libc_fn_t    initialize_libc;
	awsm_abi_entrypoint_fn_t   entrypoint;
};

/* Initializes the ABI object using the *.so file at path */
static inline int
awsm_abi_init(struct awsm_abi *abi, char *path)
{
	assert(abi != NULL);

	int rc = 0;

	abi->handle = dlopen(path, RTLD_LAZY | RTLD_DEEPBIND);
	if (abi->handle == NULL) {
		fprintf(stderr, "Failed to open %s with error: %s\n", path, dlerror());
		goto dl_open_error;
	};

	/* Resolve the symbols in the dynamic library *.so file */
	abi->entrypoint = (awsm_abi_entrypoint_fn_t)dlsym(abi->handle, AWSM_ABI_ENTRYPOINT);
	if (abi->entrypoint == NULL) {
		fprintf(stderr, "Failed to resolve symbol %s in %s with error: %s\n", AWSM_ABI_ENTRYPOINT, path,
		        dlerror());
		goto dl_error;
	}

	/*
	 * This symbol may or may not be present depending on whether the aWsm was
	 * run with the --runtime-globals flag. It is not clear what the proper
	 * configuration would be for SLEdge, so no validation is performed
	 */
	abi->initialize_globals = (awsm_abi_init_globals_fn_t)dlsym(abi->handle, AWSM_ABI_INITIALIZE_GLOBALS);

	abi->initialize_memory = (awsm_abi_init_mem_fn_t)dlsym(abi->handle, AWSM_ABI_INITIALIZE_MEMORY);
	if (abi->initialize_memory == NULL) {
		fprintf(stderr, "Failed to resolve symbol %s in %s with error: %s\n", AWSM_ABI_INITIALIZE_MEMORY, path,
		        dlerror());
		goto dl_error;
	};

	abi->initialize_tables = (awsm_abi_init_tbl_fn_t)dlsym(abi->handle, AWSM_ABI_INITIALIZE_TABLE);
	if (abi->initialize_tables == NULL) {
		fprintf(stderr, "Failed to resolve symbol %s in %s with error: %s\n", AWSM_ABI_INITIALIZE_TABLE, path,
		        dlerror());
		goto dl_error;
	};

	abi->initialize_libc = (awsm_abi_init_libc_fn_t)dlsym(abi->handle, AWSM_ABI_INITIALIZE_LIBC);
	if (abi->initialize_libc == NULL) {
		fprintf(stderr, "Failed to resolve symbol %s in %s with error: %s\n", AWSM_ABI_INITIALIZE_LIBC, path,
		        dlerror());
		goto dl_error;
	}

done:
	return rc;
dl_error:
	dlclose(abi->handle);
dl_open_error:
	rc = -1;
	goto done;
}

static inline int
awsm_abi_deinit(struct awsm_abi *abi)
{
	abi->entrypoint         = NULL;
	abi->initialize_globals = NULL;
	abi->initialize_memory  = NULL;
	abi->initialize_tables  = NULL;
	abi->initialize_libc    = NULL;

	int rc = dlclose(abi->handle);
	if (rc != 0) {
		fprintf(stderr, "Failed to close *.so file with error: %s\n", dlerror());
		return 1;
	}

	return 0;
}