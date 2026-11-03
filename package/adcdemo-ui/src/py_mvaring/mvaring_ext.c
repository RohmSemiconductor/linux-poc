/* mvaring_ext.c - Python C extension for reading mvaring ring buffer from shared memory
 *
 * The ring buffer is payload agnostic, and so is this module: items are
 * opaque byte blocks of item_size bytes (stored in the ring header). The
 * caller knows the layout and may pass the item size it expects to open().
 *
 * Exposes a minimal consumer API:
 *   mvaring.open(shm_name, item_size=0) -> opaque handle (capsule)
 *   mvaring.close(handle)          -> None
 *   mvaring.is_ok(handle)          -> bool
 *   mvaring.available(handle)      -> int
 *   mvaring.item_size(handle)      -> int
 *   mvaring.num_slots(handle)      -> int
 *   mvaring.read(handle, max_items) -> bytes
 *
 * read() returns the items back to back in one bytes object whose length is
 * a multiple of item_size(). Use memoryview/struct/array to decode them.
 *
 * NOTE: ring_read() advances rindex in shared memory, so this is a proper
 * consuming reader. Only ONE reader process should consume from the ring
 * at a time; multiple concurrent readers will corrupt rindex.
 */

#define _GNU_SOURCE
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>

#include "mvaring.h"
#include "shmem.h"

typedef struct
{
	struct shmem_info shmem;
	struct mvaring *ring;
} mvaring_ctx_t;

/* Called by Python GC when the capsule is collected (or after explicit close). */
static void ctx_destructor(PyObject *capsule)
{
	mvaring_ctx_t *ctx = (mvaring_ctx_t *)PyCapsule_GetPointer(capsule, "mvaring.ctx");
	if (!ctx)
		return;

	/* shmem_close is a no-op when buff is NULL (already closed manually) */
	shmem_close(&ctx->shmem);
	free((void *)ctx->shmem.name);
	free(ctx);
}

/* Release a half initialised context */
static void ctx_free(mvaring_ctx_t *ctx)
{
	shmem_close(&ctx->shmem);
	free((void *)ctx->shmem.name);
	free(ctx);
}

/* mvaring.open(shm_name: str, item_size: int = 0) -> handle
 *
 * Opens the POSIX shared memory segment and validates that it contains a
 * well-formed mvaring ring buffer.  The shm_name must start with '/'
 * (e.g. "/RPI_ADC_BUFF").  If item_size is non-zero the ring must have
 * exactly that item size.
 *
 * Raises OSError on shm_open/mmap failure, ValueError on bad ring buffer.
 */
static PyObject *py_mvaring_open(PyObject *self, PyObject *args)
{
	const char *name;
	unsigned long want_isz = 0;
	size_t ring_size;
	struct stat st;

	if (!PyArg_ParseTuple(args, "s|k", &name, &want_isz))
		return NULL;

	mvaring_ctx_t *ctx = (mvaring_ctx_t *)calloc(1, sizeof(*ctx));
	if (!ctx)
		return PyErr_NoMemory();

	/*
	 * Open read-write: ring_read() must advance rindex in shared memory.
	 * From the Python caller's perspective this is still "read-only" —
	 * only ADC data is read, never written.
	 *
	 * The ring size is only known from the header, so map the header
	 * first, check it, and then map the whole ring.
	 */
	int ret = shmem_open(name, sizeof(struct mvaring), &ctx->shmem, /*quiet=*/false);
	if (ret < 0)
	{
		free(ctx);
		errno = -ret;
		return PyErr_SetFromErrnoWithFilename(PyExc_OSError, name);
	}

	ctx->ring = (struct mvaring *)ctx->shmem.buff;
	ring_size = ctx->ring->size;

	if (!ring_is_ok(ctx->ring) || fstat(ctx->shmem.fd, &st) != 0 ||
		(size_t)st.st_size < ring_size)
	{
		ctx_free(ctx);
		PyErr_Format(PyExc_ValueError,
					 "Shared memory '%s' does not contain a valid mvaring ring buffer "
					 "(expected version %d)",
					 name, MVARING_VERSION);
		return NULL;
	}

	if (want_isz && ring_item_size(ctx->ring) != want_isz)
	{
		size_t isz = ring_item_size(ctx->ring);

		ctx_free(ctx);
		PyErr_Format(PyExc_ValueError,
					 "Ring buffer '%s' item size is %zu, expected %lu",
					 name, isz, want_isz);
		return NULL;
	}

	shmem_close(&ctx->shmem);
	free((void *)ctx->shmem.name);
	ret = shmem_open(name, ring_size, &ctx->shmem, /*quiet=*/false);
	if (ret < 0)
	{
		free(ctx);
		errno = -ret;
		return PyErr_SetFromErrnoWithFilename(PyExc_OSError, name);
	}

	ctx->ring = (struct mvaring *)ctx->shmem.buff;

	return PyCapsule_New(ctx, "mvaring.ctx", ctx_destructor);
}

/* mvaring.close(handle) -> None
 *
 * Explicitly releases the shared memory mapping.  The handle becomes invalid
 * after this call; subsequent use raises ValueError.
 */
static PyObject *py_mvaring_close(PyObject *self, PyObject *args)
{
	PyObject *capsule;

	if (!PyArg_ParseTuple(args, "O", &capsule))
		return NULL;

	mvaring_ctx_t *ctx = (mvaring_ctx_t *)PyCapsule_GetPointer(capsule, "mvaring.ctx");
	if (!ctx)
	{
		PyErr_SetString(PyExc_ValueError, "Invalid or already-closed mvaring handle");
		return NULL;
	}

	shmem_close(&ctx->shmem);
	free((void *)ctx->shmem.name);

	/* Zero out pointers so the GC destructor is a safe no-op */
	ctx->shmem.buff = NULL;
	ctx->shmem.name = NULL;
	ctx->ring = NULL;

	Py_RETURN_NONE;
}

/* mvaring.is_ok(handle) -> bool
 *
 * Returns True if the ring buffer header looks valid (correct version + size).
 */
static PyObject *py_mvaring_is_ok(PyObject *self, PyObject *args)
{
	PyObject *capsule;

	if (!PyArg_ParseTuple(args, "O", &capsule))
		return NULL;

	mvaring_ctx_t *ctx = (mvaring_ctx_t *)PyCapsule_GetPointer(capsule, "mvaring.ctx");
	if (!ctx)
	{
		PyErr_SetString(PyExc_ValueError, "Invalid or already-closed mvaring handle");
		return NULL;
	}

	return PyBool_FromLong(ring_is_ok(ctx->ring));
}

/* mvaring.available(handle) -> int
 *
 * Returns the number of chunks currently available for reading.
 * This is a snapshot; the value may change immediately due to the writer.
 */
static PyObject *py_mvaring_available(PyObject *self, PyObject *args)
{
	PyObject *capsule;

	if (!PyArg_ParseTuple(args, "O", &capsule))
		return NULL;

	mvaring_ctx_t *ctx = (mvaring_ctx_t *)PyCapsule_GetPointer(capsule, "mvaring.ctx");
	if (!ctx)
	{
		PyErr_SetString(PyExc_ValueError, "Invalid or already-closed mvaring handle");
		return NULL;
	}

	return PyLong_FromUnsignedLong(ring_available(ctx->ring));
}

/* mvaring.item_size(handle) -> int
 * mvaring.num_slots(handle) -> int
 *
 * Item size in bytes and slot count, as stored in the ring header.
 */
static PyObject *py_mvaring_item_size(PyObject *self, PyObject *args)
{
	PyObject *capsule;

	if (!PyArg_ParseTuple(args, "O", &capsule))
		return NULL;

	mvaring_ctx_t *ctx = (mvaring_ctx_t *)PyCapsule_GetPointer(capsule, "mvaring.ctx");
	if (!ctx || !ctx->ring)
	{
		PyErr_SetString(PyExc_ValueError, "Invalid or already-closed mvaring handle");
		return NULL;
	}

	return PyLong_FromSize_t(ring_item_size(ctx->ring));
}

static PyObject *py_mvaring_num_slots(PyObject *self, PyObject *args)
{
	PyObject *capsule;

	if (!PyArg_ParseTuple(args, "O", &capsule))
		return NULL;

	mvaring_ctx_t *ctx = (mvaring_ctx_t *)PyCapsule_GetPointer(capsule, "mvaring.ctx");
	if (!ctx || !ctx->ring)
	{
		PyErr_SetString(PyExc_ValueError, "Invalid or already-closed mvaring handle");
		return NULL;
	}

	return PyLong_FromUnsignedLong(ctx->ring->num_slots);
}

/* mvaring.read(handle, max_items: int = 64) -> bytes
 *
 * Reads up to max_items items from the ring buffer, advancing rindex.
 * Returns an empty bytes object when no data is available (non-blocking).
 * The items are returned back to back, so the length is n * item_size().
 * The data is copied once, straight from the ring into the bytes object.
 *
 * Raises OSError on unexpected error (-EINVAL from ring_read).
 */
static PyObject *py_mvaring_read(PyObject *self, PyObject *args)
{
	PyObject *capsule;
	unsigned int max_items = 64;

	if (!PyArg_ParseTuple(args, "O|I", &capsule, &max_items))
		return NULL;

	mvaring_ctx_t *ctx = (mvaring_ctx_t *)PyCapsule_GetPointer(capsule, "mvaring.ctx");
	if (!ctx || !ctx->ring)
	{
		PyErr_SetString(PyExc_ValueError, "Invalid or already-closed mvaring handle");
		return NULL;
	}

	/*
	 * Only one reader exists, so the number of available items can only
	 * grow after this check; reading n of them is safe.
	 */
	size_t isz = ring_item_size(ctx->ring);
	unsigned int n = ring_available(ctx->ring);

	if (max_items == 0 || max_items > ctx->ring->num_slots)
		max_items = ctx->ring->num_slots;
	if (n > max_items)
		n = max_items;
	if (n == 0)
		return PyBytes_FromStringAndSize(NULL, 0);

	PyObject *result = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)(n * isz));
	if (!result)
		return NULL;

	int got = ring_read(ctx->ring, PyBytes_AS_STRING(result), n);

	if (got == -EAGAIN)
		got = 0; /* writer busy — non-blocking return */

	if (got < 0)
	{
		Py_DECREF(result);
		errno = -got;
		return PyErr_SetFromErrno(PyExc_OSError);
	}

	if ((unsigned int)got != n && _PyBytes_Resize(&result, (Py_ssize_t)(got * isz)) < 0)
		return NULL;

	return result;
}

/* -------------------------------------------------------------------------- */

static PyMethodDef mvaring_methods[] = {
	{"open", py_mvaring_open, METH_VARARGS,
	 "open(shm_name, item_size=0) -> handle\n\n"
	 "Open a POSIX shared memory segment and attach to the mvaring ring buffer.\n"
	 "shm_name must start with '/' (e.g. '/RPI_ADC_BUFF'). A non-zero item_size\n"
	 "must match the item size in the ring header (ValueError otherwise)."},

	{"close", py_mvaring_close, METH_VARARGS,
	 "close(handle) -> None\n\nRelease the shared memory mapping."},

	{"is_ok", py_mvaring_is_ok, METH_VARARGS,
	 "is_ok(handle) -> bool\n\nReturn True if the ring buffer header is valid."},

	{"available", py_mvaring_available, METH_VARARGS,
	 "available(handle) -> int\n\nNumber of items currently available for reading."},

	{"item_size", py_mvaring_item_size, METH_VARARGS,
	 "item_size(handle) -> int\n\nSize of one ring item in bytes."},

	{"num_slots", py_mvaring_num_slots, METH_VARARGS,
	 "num_slots(handle) -> int\n\nNumber of slots in the ring (capacity is one less)."},

	{"read", py_mvaring_read, METH_VARARGS,
	 "read(handle, max_items=64) -> bytes\n\n"
	 "Read up to max_items items from the ring buffer (consuming).\n"
	 "Returns the items back to back (n * item_size bytes), or b'' when no\n"
	 "data is available."},

	{NULL, NULL, 0, NULL}};

static struct PyModuleDef mvaring_module = {
	PyModuleDef_HEAD_INIT,
	"mvaring",
	"Python bindings for the mvaring lock-free ring buffer (read-only consumer).",
	-1,
	mvaring_methods};

PyMODINIT_FUNC PyInit_mvaring(void)
{
	PyObject *m = PyModule_Create(&mvaring_module);
	if (!m)
		return NULL;

	/* Expose the ring header version this module understands */
	PyModule_AddIntConstant(m, "MVARING_VERSION", MVARING_VERSION);

	return m;
}
