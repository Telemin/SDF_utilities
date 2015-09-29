#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION

#include <float.h>
#include <Python.h>
#include <numpy/arrayobject.h>
#include <structmember.h>
#include "sdf.h"
#include "sdf_extension.h"
#include "sdf_helper.h"
#include "stack_allocator.h"
#include "commit_info.h"

/* Backwards compatibility */

#if PY_MAJOR_VERSION < 3
    #define PyInt_FromLong PyLong_FromLong
    #define PyBytes_FromString PyString_FromString
    #define PyBytes_AsString PyString_AsString
#endif

#ifndef NPY_ARRAY_F_CONTIGUOUS
    #define NPY_ARRAY_F_CONTIGUOUS NPY_F_CONTIGUOUS
#endif

#ifndef PyVarObject_HEAD_INIT
    #define PyVarObject_HEAD_INIT(type, size) \
        PyObject_HEAD_INIT(type) size,
#endif

#ifndef PyArray_SetBaseObject
    #define PyArray_SetBaseObject(array, base) \
             PyArray_BASE(array) = base
#endif

#if PY_MAJOR_VERSION >= 3
    #define MOD_ERROR_VAL NULL
    #define MOD_SUCCESS_VAL(val) val
    #define MOD_INIT(name) PyMODINIT_FUNC PyInit_##name(void)
    #define MOD_DEF(ob, name, doc, methods) \
        static struct PyModuleDef moduledef = { \
            PyModuleDef_HEAD_INIT, name, doc, -1, methods, }; \
        ob = PyModule_Create(&moduledef);
#else
    #define MOD_ERROR_VAL
    #define MOD_SUCCESS_VAL(val)
    #define MOD_INIT(name) void init##name(void)
    #define MOD_DEF(ob, name, doc, methods) \
        ob = Py_InitModule3(name, methods, doc);
#endif

#define IJ(i,j) ((i) + (j) * (block->adims[0] + 1))

int sdf_free_block_data(sdf_file_t *h, sdf_block_t *b);

static const int typemap[] = {
    0,
    NPY_UINT32,
    NPY_UINT64,
    NPY_FLOAT,
    NPY_DOUBLE,
#ifdef NPY_FLOAT128
    NPY_FLOAT128,
#else
    0,
#endif
    NPY_CHAR,
    NPY_BOOL,
    NPY_VOID,
};


typedef struct {
    PyObject_HEAD
    sdf_file_t *h;
} SDFObject;


typedef struct {
    PyObject_HEAD
    SDFObject *sdf;
    sdf_block_t *b;
    void **mem;
    int memlen;
} ArrayObject;


typedef struct Block_struct Block;

struct Block_struct {
    PyObject_HEAD
    PyObject *id;
    PyObject *name;
    PyObject *data_length;
    PyObject *datatype;
    PyObject *dims;
    PyObject *data;
    PyObject *labels;
    PyObject *units;
    PyObject *extents;
    PyObject *geometry;
    PyObject *species_id;
    PyObject *mult;
    PyObject *stagger;
    PyObject *dict;
    PyObject *material_names;
    PyObject *material_ids;
    PyObject *grid_id;
    Block *grid;
    Block *grid_mid;
    Block *parent;
    SDFObject *sdf;
    sdf_block_t *b;
    int dim, ndims;
    int sdfref;
    npy_intp adims[4];
};


typedef struct {
    PyObject_HEAD
    PyObject *dict;
} BlockList;


static PyTypeObject ArrayType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "Array",                   /* tp_name           */
    sizeof(ArrayObject),       /* tp_basicsize      */
    0,                         /* tp_itemsize       */
};


static PyTypeObject BlockType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "sdf.Block",               /* tp_name           */
    sizeof(Block),             /* tp_basicsize      */
    0,                         /* tp_itemsize       */
};


static PyTypeObject BlockListType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "sdf.BlockList",           /* tp_name           */
    sizeof(BlockList),         /* tp_basicsize      */
    0,                         /* tp_itemsize       */
};


static PyTypeObject BlockBase;
static PyTypeObject BlockMeshType;
static PyTypeObject BlockPlainMeshType;
static PyTypeObject BlockPointMeshType;
static PyTypeObject BlockLagrangianMeshType;
static PyTypeObject BlockPlainVariableType;
static PyTypeObject BlockPointVariableType;
static PyTypeObject BlockArrayType;
static PyTypeObject BlockConstantType;
static PyTypeObject BlockStationType;
static PyTypeObject BlockStitchedMaterialType;
static PyTypeObject BlockNameValueType;


static PyTypeObject SDFType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "sdf.SDF",                 /* tp_name           */
    sizeof(SDFObject),         /* tp_basicsize      */
    0,                         /* tp_itemsize       */
};


/*
 * Array type methods
 ******************************************************************************/

static PyObject *
Array_new(PyTypeObject *type, SDFObject *sdf, sdf_block_t *b)
{
    ArrayObject *self;

    self = (ArrayObject*)type->tp_alloc(type, 0);
    if (self) {
        self->sdf = sdf;
        self->b = b;
        self->mem = NULL;
        self->memlen = 0;
        Py_INCREF(self->sdf);
    }

    return (PyObject*)self;
}


static void
Array_dealloc(PyObject *self)
{
    ArrayObject *ob = (ArrayObject*)self;
    Py_ssize_t n;

    if (!ob) return;

    if (ob->memlen) {
        for (n = 0; n < ob->memlen; n++)
            free(ob->mem[n]);
        free(ob->mem);
        ob->memlen = 0;
    } else
        sdf_free_block_data(ob->sdf->h, ob->b);

    Py_XDECREF(ob->sdf);
    self->ob_type->tp_free(self);
}


/*
 * BlockList type methods
 ******************************************************************************/

static PyMemberDef BlockList_members[] = {
    {"__dict__", T_OBJECT, offsetof(BlockList, dict), READONLY},
    {0}
};


static void
BlockList_dealloc(PyObject *self)
{
    BlockList *ob = (BlockList*)self;

    Py_XDECREF(ob->dict);
    self->ob_type->tp_free(self);
}


/*
 * Block type methods
 ******************************************************************************/

static PyMemberDef Block_members[] = {
    {"id", T_OBJECT_EX, offsetof(Block, id), 0, "Block id"},
    {"name", T_OBJECT_EX, offsetof(Block, name), 0, "Block name"},
    {"data_length", T_OBJECT_EX, offsetof(Block, data_length), 0, "Data size"},
    {"datatype", T_OBJECT_EX, offsetof(Block, datatype), 0, "Data type"},
    {"dims", T_OBJECT_EX, offsetof(Block, dims), 0, "Data dimensions"},
    {"data", T_OBJECT_EX, offsetof(Block, data), 0, "Block data contents"},
    {"__dict__", T_OBJECT_EX, offsetof(Block, dict), READONLY},
    {NULL}  /* Sentinel */
};

static PyMemberDef BlockMesh_members[] = {
    {"labels", T_OBJECT_EX, offsetof(Block, labels), 0, "Axis labels"},
    {"units", T_OBJECT_EX, offsetof(Block, units), 0, "Axis units"},
    {"extents", T_OBJECT_EX, offsetof(Block, extents), 0, "Axis extents"},
    {"geometry", T_OBJECT_EX, offsetof(Block, geometry), 0, "Domain geometry"},
    {"mult", T_OBJECT_EX, offsetof(Block, mult), 0, "Multiplication factors"},
    {NULL}  /* Sentinel */
};

static PyMemberDef BlockMeshVariable_members[] = {
    {"grid", T_OBJECT_EX, offsetof(Block, grid), 0, "Associated mesh"},
    {"grid_mid", T_OBJECT_EX, offsetof(Block, grid_mid), 0,
     "Associated median mesh"},
    {"grid_id", T_OBJECT_EX, offsetof(Block, grid_id), 0, "Associated mesh id"},
    {"units", T_OBJECT_EX, offsetof(Block, units), 0, "Units of variable"},
    {"mult", T_OBJECT_EX, offsetof(Block, mult), 0, "Multiplication factor"},
    {NULL}  /* Sentinel */
};

static PyMemberDef BlockPointVariable_members[] = {
    {"grid", T_OBJECT_EX, offsetof(Block, grid), 0, "Associated mesh"},
    {"grid_mid", T_OBJECT_EX, offsetof(Block, grid_mid), 0,
     "Associated median mesh"},
    {"grid_id", T_OBJECT_EX, offsetof(Block, grid_id), 0, "Associated mesh id"},
    {"units", T_OBJECT_EX, offsetof(Block, units), 0, "Units of variable"},
    {"mult", T_OBJECT_EX, offsetof(Block, mult), 0, "Multiplication factor"},
    {"species_id", T_OBJECT_EX, offsetof(Block, species_id), 0, "Species ID"},
    {NULL}  /* Sentinel */
};

static PyMemberDef BlockPlainMesh_members[] = {
    {"stagger", T_OBJECT_EX, offsetof(Block, stagger), 0, "Grid stagger"},
    {NULL}  /* Sentinel */
};

static PyMemberDef BlockPointMesh_members[] = {
    {"species_id", T_OBJECT_EX, offsetof(Block, species_id), 0, "Species ID"},
    {NULL}  /* Sentinel */
};

static PyMemberDef BlockStitchedMaterial_members[] = {
    {"stagger", T_OBJECT_EX, offsetof(Block, stagger), 0, "Grid stagger"},
    {"material_names", T_OBJECT_EX, offsetof(Block, material_names), 0,
     "Material names"},
    {"material_ids", T_OBJECT_EX, offsetof(Block, material_ids), 0,
     "Material IDs"},
    {"grid", T_OBJECT_EX, offsetof(Block, grid), 0, "Associated mesh"},
    {"grid_mid", T_OBJECT_EX, offsetof(Block, grid_mid), 0,
     "Associated median mesh"},
    {"grid_id", T_OBJECT_EX, offsetof(Block, grid_id), 0, "Associated mesh id"},
    {NULL}  /* Sentinel */
};


static PyObject *Block_getdata(Block *block, void *closure);
static int Block_setdata(Block *block, PyObject *value, void *closure);

static PyGetSetDef Block_getset[] = {
    {"data", (getter)Block_getdata, (setter)Block_setdata,
     "Block data contents", NULL},
    {NULL}  /* Sentinel */
};


static PyObject *
Block_alloc(SDFObject *sdf, sdf_block_t *b)
{
    Block *ob;
    PyTypeObject *type;
    Py_ssize_t i;

    if (!b->datatype_out) return NULL;

    switch(b->blocktype) {
        case SDF_BLOCKTYPE_PLAIN_MESH:
            type = &BlockPlainMeshType;
            break;
        case SDF_BLOCKTYPE_POINT_MESH:
            type = &BlockPointMeshType;
            break;
        case SDF_BLOCKTYPE_LAGRANGIAN_MESH:
            type = &BlockLagrangianMeshType;
            break;
        case SDF_BLOCKTYPE_PLAIN_VARIABLE:
        case SDF_BLOCKTYPE_PLAIN_DERIVED:
            type = &BlockPlainVariableType;
            break;
        case SDF_BLOCKTYPE_POINT_VARIABLE:
        case SDF_BLOCKTYPE_POINT_DERIVED:
            type = &BlockPointVariableType;
            break;
        case SDF_BLOCKTYPE_ARRAY:
            type = &BlockArrayType;
            break;
        case SDF_BLOCKTYPE_CONSTANT:
            type = &BlockConstantType;
            break;
        case SDF_BLOCKTYPE_STATION:
            type = &BlockStationType;
            break;
        case SDF_BLOCKTYPE_STITCHED_MATERIAL:
        case SDF_BLOCKTYPE_CONTIGUOUS_MATERIAL:
            type = &BlockStitchedMaterialType;
            break;
        case SDF_BLOCKTYPE_NAMEVALUE:
            type = &BlockNameValueType;
            break;
        default:
            type = &BlockType;
    }

    ob = (Block *)type->tp_alloc(type, 0);
    ob->sdf = sdf;
    ob->b = b;
    ob->dim = 0;
    ob->ndims = b->ndims;
    ob->sdfref = 0;

    if (b->id) {
        ob->id = PyBytes_FromString(b->id);
        if (!ob->id) goto error;
    }

    if (b->name) {
        ob->name = PyBytes_FromString(b->name);
        if (!ob->name) goto error;
    }

    if (b->mesh_id) {
        ob->grid_id = PyBytes_FromString(b->mesh_id);
        if (!ob->grid_id) goto error;
    }

    ob->data_length = PyLong_FromLongLong(b->data_length);
    if (!ob->data_length) goto error;

    ob->datatype = PyArray_TypeObjectFromType(typemap[b->datatype_out]);
    if (!ob->datatype) goto error;

    if (b->ndims) {
        if (b->blocktype == SDF_BLOCKTYPE_NAMEVALUE)
            ob->dims = PyTuple_New(1);
        else
            ob->dims = PyTuple_New(b->ndims);
        if (!ob->dims) goto error;
    }

    if (b->extents) {
        ob->extents = PyTuple_New(2 * b->ndims);
        if (!ob->extents) goto error;
        for (i=0; i < 2 * b->ndims; i++) {
            PyTuple_SetItem(ob->extents, i, PyFloat_FromDouble(b->extents[i]));
        }
    }

    switch(b->blocktype) {
        case SDF_BLOCKTYPE_PLAIN_MESH:
        case SDF_BLOCKTYPE_POINT_MESH:
        case SDF_BLOCKTYPE_LAGRANGIAN_MESH:
            ob->geometry = PyLong_FromLong(b->geometry);
            if (!ob->geometry) goto error;
        case SDF_BLOCKTYPE_PLAIN_VARIABLE:
        case SDF_BLOCKTYPE_PLAIN_DERIVED:
        case SDF_BLOCKTYPE_POINT_VARIABLE:
        case SDF_BLOCKTYPE_POINT_DERIVED:
        case SDF_BLOCKTYPE_ARRAY:
            ob->sdfref = 1;
            Py_INCREF(ob->sdf);
            if (b->dims && ob->dims) {
                for (i=0; i < b->ndims; i++) {
                    ob->adims[i] = b->dims[i];
                    PyTuple_SetItem(ob->dims, i, PyLong_FromLong(b->dims[i]));
                }
            }
            break;
        case SDF_BLOCKTYPE_NAMEVALUE:
            PyTuple_SetItem(ob->dims, 0, PyLong_FromLong(b->ndims));
            break;
        case SDF_BLOCKTYPE_CONSTANT:
            PyTuple_SetItem(ob->dims, 0, PyLong_FromLong(1));
            break;
    }

    switch(b->blocktype) {
        case SDF_BLOCKTYPE_POINT_MESH:
        case SDF_BLOCKTYPE_POINT_VARIABLE:
        case SDF_BLOCKTYPE_POINT_DERIVED:
            if (b->material_id) {
                ob->species_id = PyBytes_FromString(b->material_id);
                if (!ob->species_id) goto error;
            }
            break;
    }

    switch(b->blocktype) {
        case SDF_BLOCKTYPE_PLAIN_VARIABLE:
        case SDF_BLOCKTYPE_PLAIN_DERIVED:
        case SDF_BLOCKTYPE_POINT_VARIABLE:
        case SDF_BLOCKTYPE_POINT_DERIVED:
            if (b->mult) {
                ob->mult = PyFloat_FromDouble(b->mult);
                if (!ob->mult) goto error;
            }
            break;
        case SDF_BLOCKTYPE_PLAIN_MESH:
        case SDF_BLOCKTYPE_POINT_MESH:
            if (b->dim_mults) {
                ob->mult = PyTuple_New(b->ndims);
                if (!ob->mult) goto error;
                for (i=0; i < b->ndims; i++) {
                    PyTuple_SetItem(ob->mult, i,
                                    PyFloat_FromDouble(b->dim_mults[i]));
                }
            }
            break;
    }

    switch(b->blocktype) {
        case SDF_BLOCKTYPE_PLAIN_MESH:
        case SDF_BLOCKTYPE_LAGRANGIAN_MESH:
        case SDF_BLOCKTYPE_STITCHED_MATERIAL:
        case SDF_BLOCKTYPE_CONTIGUOUS_MATERIAL:
            ob->stagger = PyLong_FromLong(b->stagger);
            if (!ob->stagger) goto error;
            break;
    }

    return (PyObject *)ob;

error:
    Py_DECREF(ob);

    return NULL;
}


static void
Block_dealloc(PyObject *self)
{
    Block *ob = (Block*)self;
    if (!ob) return;
    Py_XDECREF(ob->id);
    Py_XDECREF(ob->name);
    Py_XDECREF(ob->data_length);
    Py_XDECREF(ob->datatype);
    Py_XDECREF(ob->dims);
    Py_XDECREF(ob->extents);
    Py_XDECREF(ob->geometry);
    Py_XDECREF(ob->species_id);
    Py_XDECREF(ob->mult);
    Py_XDECREF(ob->stagger);
    Py_XDECREF(ob->labels);
    Py_XDECREF(ob->units);
    Py_XDECREF(ob->dict);
    Py_XDECREF(ob->data);
    Py_XDECREF(ob->parent);
    Py_XDECREF(ob->material_names);
    Py_XDECREF(ob->material_ids);
    Py_XDECREF(ob->grid_id);
    if (ob->sdfref > 0) {
        ob->sdfref--;
        Py_XDECREF(ob->sdf);
    }

    /* Object should not free itself unless tp_alloc has been defined */
    self->ob_type->tp_free(self);
}

/* FIXME:
 * The functions Block_getdata, Block_setdata can give the following
 * weird behaviour:
 *
 *  >>> print block.data
 *  array([0., 1., ..., 100.]
 *  >>> del block.data
 *  >>> print block.data
 *  array([0., 1., ..., 100.]
 *
 *  where we would expect an error, because block.data shouldn't
 *  exist anymore.
 *
 *  Possible solutions:
 *     - add a flag 'data_deleted', that is checked by Block_getdata
 *       before it tries to reload
 *     - replace 'block.data' with the method 'block.get_data()'
 *       that returns a numpy array that can just be deleted, without
 *       any need for Block_setdata.
 *       Obviously this solution breaks backward compatibility.
 *
 *  AMW - 2015-07-16
 */


static PyObject *Block_getdata(Block *block, void *closure)
{
    void *data;
    SDFObject *sdf = block->sdf;
    sdf_block_t *b = block->b;
    PyObject *ob;
    ArrayObject *array = NULL;
    Py_ssize_t ndims, n, i, j;
    npy_intp *dims;
    npy_intp adims[1];
    void *mem;
    int float1d = 0, double1d = 0, float2d = 0, double2d = 0;

    /* Already populated numpy array. Just return it. */
    if (block->data) {
        Py_INCREF(block->data);
        return block->data;
    }

    if (block->parent) {
        ob = Block_getdata(block->parent, closure);
        Py_DECREF(ob);
    }

    if (!sdf || !sdf->h || !b)
        return PyErr_Format(PyExc_Exception, "Unknown SDF file\n");

    sdf->h->current_block = b;
    sdf_helper_read_data(sdf->h, b);

    if (b->grids && b->grids[0])
        data = b->grids[0];
    else
        data = b->data;

    if (!data)
        return PyErr_Format(PyExc_Exception, "Unable to read SDF block\n");

    dims = block->adims;
    ndims = b->ndims;

    if (b->grids && b->grids[0]) {
        block->data = PyTuple_New(b->ndims);
        if (!block->data) goto free_mem;

        array = (ArrayObject*)Array_new(&ArrayType, sdf, b);
        if (!array) goto free_mem;

        if (block->parent) {
            array->memlen = b->ndims;
            array->mem = malloc(array->memlen * sizeof(*array->mem));
        }

        if (b->blocktype == SDF_BLOCKTYPE_PLAIN_MESH
                || b->blocktype == SDF_BLOCKTYPE_POINT_MESH) {
            ndims = 1;
            dims = adims;
            if (block->parent) {
                if (b->datatype_out == SDF_DATATYPE_REAL4)
                    float1d = 1;
                else
                    double1d = 1;
            }
        } else {
            if (block->parent) {
                if (b->datatype_out == SDF_DATATYPE_REAL4)
                    float2d = 1;
                else
                    double2d = 1;
            }
        }

        for (n = 0; n < b->ndims; n++) {
            data = b->grids[n];
            mem = NULL;

            if (float1d) {
                float v1, v2, *ptr_in, *ptr_out;
                Py_ssize_t ntot = block->adims[n];
                ptr_in = data;
                ptr_out = data = mem = malloc(ntot * sizeof(*ptr_out));
                if (!mem) goto free_mem;
                for (i = 0; i < ntot; i++) {
                    v1 = *ptr_in;
                    v2 = *(ptr_in+1);
                    *ptr_out++ = 0.5 * (v1 + v2);
                    ptr_in++;
                }
            } else if (double1d) {
                double v1, v2, *ptr_in, *ptr_out;
                Py_ssize_t ntot = block->adims[n];
                ptr_in = data;
                ptr_out = data = mem = malloc(ntot * sizeof(*ptr_out));
                if (!mem) goto free_mem;
                for (i = 0; i < ntot; i++) {
                    v1 = *ptr_in;
                    v2 = *(ptr_in+1);
                    *ptr_out++ = 0.5 * (v1 + v2);
                    ptr_in++;
                }
            } else if (float2d) {
                float *ptr_in, *ptr_out;
                Py_ssize_t ntot = block->adims[0] * block->adims[1];
                ptr_in = data;
                ptr_out = data = mem = malloc(ntot * sizeof(*ptr_out));
                if (!mem) goto free_mem;
                for (j = 0; j < block->adims[1]; j++) {
                for (i = 0; i < block->adims[0]; i++) {
                    *ptr_out++ = 0.25 * (ptr_in[IJ(i,j)] + ptr_in[IJ(i+1,j)]
                                  + ptr_in[IJ(i,j+1)] + ptr_in[IJ(i+1,j+1)]);
                }}
            } else if (double2d) {
                double *ptr_in, *ptr_out;
                Py_ssize_t ntot = block->adims[0] * block->adims[1];
                ptr_in = data;
                ptr_out = data = mem = malloc(ntot * sizeof(*ptr_out));
                if (!mem) goto free_mem;
                for (j = 0; j < block->adims[1]; j++) {
                for (i = 0; i < block->adims[0]; i++) {
                    *ptr_out++ = 0.25 * (ptr_in[IJ(i,j)] + ptr_in[IJ(i+1,j)]
                                  + ptr_in[IJ(i,j+1)] + ptr_in[IJ(i+1,j+1)]);
                }}
            }

            if (mem)
                array->mem[n] = mem;

            if (b->blocktype == SDF_BLOCKTYPE_PLAIN_MESH
                    || b->blocktype == SDF_BLOCKTYPE_POINT_MESH)
                dims[0] = block->adims[n];

            ob = PyArray_NewFromDescr(&PyArray_Type,
                PyArray_DescrFromType(typemap[b->datatype_out]), ndims,
                dims, NULL, data, NPY_ARRAY_F_CONTIGUOUS, NULL);
            if (!ob) goto free_mem;

            PyTuple_SetItem(block->data, n, ob);

            PyArray_SetBaseObject((PyArrayObject*)ob, (PyObject*)array);
            Py_INCREF(array);
        }
        Py_DECREF(array);
    } else {
        block->data = PyArray_NewFromDescr(&PyArray_Type,
            PyArray_DescrFromType(typemap[b->datatype_out]), ndims,
            dims, NULL, data, NPY_ARRAY_F_CONTIGUOUS, NULL);
        if (!block->data) goto free_mem;

        array = (ArrayObject*)Array_new(&ArrayType, sdf, b);
        if (!array) goto free_mem;

        PyArray_SetBaseObject((PyArrayObject*)block->data, (PyObject*)array);
    }

    Py_INCREF(block->data);
    return block->data;

free_mem:
    if (block->data) Py_DECREF(block->data);
    if (array) Py_DECREF(array);
    sdf_free_block_data(sdf->h, b);
    return PyErr_Format(PyExc_Exception, "Error whilst reading SDF block\n");
}


static int
Block_setdata(Block *block, PyObject *value, void *closure)
{
    Py_XDECREF(block->data);
    if ( value != NULL )
        Py_INCREF(value);
    block->data = value;

    return 0;
}


/*
 * SDF type methods
 ******************************************************************************/

static void
SDF_dealloc(PyObject* self)
{
    sdf_file_t *h = ((SDFObject*)self)->h;
    if (h) {
        sdf_stack_destroy(h);
        sdf_close(h);
    }
    self->ob_type->tp_free(self);
}


static void
setup_mesh(SDFObject *sdf, PyObject *dict, sdf_block_t *b)
{
    char *block_name = NULL;
    char *mesh_id = NULL;
    PyObject *ob = NULL;
    Block *parent, *block = NULL;
    Py_ssize_t n, len_name, len_id;

    if (!sdf->h || !b) return;

    len_name = strlen(b->name);
    block_name = malloc(len_name + 5);
    if (!block_name) goto free_mem;

    memcpy(block_name, b->name, len_name);
    block_name[len_name] = '\0';

    block = (Block*)Block_alloc(sdf, b);
    if (!block) goto free_mem;

    block->labels = PyTuple_New(b->ndims);
    if (!block->labels) goto free_mem;

    block->units = PyTuple_New(b->ndims);
    if (!block->units) goto free_mem;

    for (n = 0; n < b->ndims; n++) {
        ob = PyBytes_FromString(b->dim_labels[n]);
        if (!ob) goto free_mem;
        PyTuple_SetItem(block->labels, n, ob);

        ob = PyBytes_FromString(b->dim_units[n]);
        if (!ob) goto free_mem;
        PyTuple_SetItem(block->units, n, ob);
    }

    PyDict_SetItemString(dict, block_name, (PyObject*)block);
    Py_DECREF(block);

    if (b->blocktype == SDF_BLOCKTYPE_POINT_MESH) {
        free(block_name);
        return;
    }

    /* Add median mesh block */

    parent = block;
    memcpy(block_name+len_name, "_mid", 5);

    block = (Block*)Block_alloc(sdf, b);
    if (!block) goto free_mem;

    /* This block needs a reference to the parent and also needs the parent to
       exist as long as we do. */
    block->parent = parent;
    Py_INCREF(block->parent);

    block->labels = PyTuple_New(b->ndims);
    if (!block->labels) goto free_mem;

    block->units = PyTuple_New(b->ndims);
    if (!block->units) goto free_mem;

    for (n = 0; n < b->ndims; n++) {
        block->adims[n]--;
        PyTuple_SetItem(block->dims, n, PyLong_FromLong(block->adims[n]));

        ob = PyBytes_FromString(b->dim_labels[n]);
        if (!ob) goto free_mem;
        PyTuple_SetItem(block->labels, n, ob);

        ob = PyBytes_FromString(b->dim_units[n]);
        if (!ob) goto free_mem;
        PyTuple_SetItem(block->units, n, ob);
    }

    len_id = strlen(b->id);
    mesh_id = malloc(len_id + 5);
    if (!mesh_id) goto free_mem;

    memcpy(mesh_id, b->id, len_id);
    memcpy(mesh_id+len_id, "_mid", 5);

    Py_DECREF(block->id);
    block->id = PyBytes_FromString(mesh_id);
    if (!block->id) goto free_mem;

    free(mesh_id);

    Py_DECREF(block->name);
    block->name = PyBytes_FromString(block_name);
    if (!block->name) goto free_mem;

    PyDict_SetItemString(dict, block_name, (PyObject*)block);
    Py_DECREF(block);

    free(block_name);

    return;

free_mem:
    if (block_name) free(block_name);
    if (mesh_id) free(mesh_id);
    if (block) Py_DECREF(block);
    if (ob) Py_DECREF(ob);
}


static void extract_station_time_histories(sdf_file_t *h, PyObject *stations,
      PyObject *variables, double t0, double t1, PyObject *dict)
{
    Py_ssize_t nvars, i, nstat;
    PyObject *sub;
    char **var_names, *timehis, *v, *key;
    long *stat, ii;
    int *size, *offset, nrows, row_size;
    sdf_block_t *b;
    npy_intp dims[1];

    if ( !stations ) {
        nstat = 1;
        stat = (long *)malloc(sizeof(long));
        stat[0] = 0;
    } else {
        /* Force 'stat' to be valid input for sdf_read_station_timehis */
        nstat = PyList_Size(stations);
        stat = (long *)calloc(nstat, sizeof(long));
        nstat = 0;
        for ( ii=0; ii<h->current_block->nstations; ii++ ) {
            sub = PyLong_FromLong(ii+1);
            if ( PySequence_Contains(stations, sub) ) {
                stat[nstat] = ii;
                nstat++;
            }
            Py_DECREF(sub);
        }
    }

    if ( !nstat ) {
        free(stat);
        return;
    }

    if ( !variables ) {
        free(stat);
        return;
    }

    nvars = PyList_Size(variables);
    if ( !nvars ) {
        free(stat);
        return;
    }

    var_names = (char **)malloc(nvars*sizeof(char *));
    for ( i=0; i<nvars; i++ ) {
        sub = PyList_GetItem(variables, i);
        var_names[i] = PyBytes_AsString(sub);
        if ( !var_names[i] ) {
            free(var_names);
            free(stat);
            PyErr_SetString(PyExc_TypeError,
                    "'variables' keyword must be a string or list of strings");
            return;
        }
    }

    offset = (int *)calloc(nstat*nvars+1, sizeof(int));
    size = (int *)calloc(nstat*nvars+1, sizeof(int));
    if ( sdf_read_station_timehis(h, stat, nstat, var_names, nvars, t0, t1,
            &timehis, size, offset, &nrows, &row_size) ) {
        free(var_names);
        free(size);
        free(offset);
        free(stat);
        return;
    }

    b = h->current_block;
    key = malloc(3*h->string_length+3);
    dims[0] = nrows;

    /* Handle 'Time' as a special case */
    sub = PyArray_SimpleNewFromData(1, dims, typemap[b->variable_types[0]],
            timehis);

    sprintf(key, "%s/Time", b->name);

    PyDict_SetItemString(dict, key, sub);
    Py_DECREF(sub);

    v = timehis + nrows * size[0];
    for ( i=1; i<=nstat*nvars; i++ ) {
        if ( !size[i] )
            continue;

        sub = PyArray_SimpleNewFromData(
                1, dims, typemap[b->variable_types[i]], v);

        sprintf(key, "%s/%s/%s", b->name,
                b->station_names[stat[(int)(i-1)/nvars]],
                var_names[(i-1)%nvars]);

        PyDict_SetItemString(dict, key, sub);
        Py_DECREF(sub);

        v += nrows * size[i];
    }

    free(var_names);
    free(size);
    free(key);
    free(stat);
    free(offset);
}


int append_station_metadata(sdf_block_t *b, PyObject *dict)
{
    PyObject *block, *station, *variable;
    int i;
    Py_ssize_t j;

    /* Sanity check */
    if ( !PyDict_Check(dict) )
        return -1;

    block = PyDict_New();
    PyDict_SetItemString(dict, b->name, block);

    for ( i=0; i<b->nstations; i++ ) {
        station = PyList_New(b->station_nvars[i]);

        for ( j=0; j<b->station_nvars[i]; j++ ) {
            variable = PyBytes_FromString(b->material_names[i+j+1]);
            PyList_SET_ITEM(station, j, variable);
        }

        PyDict_SetItemString(block, b->station_names[i], station);
        Py_DECREF(station);
    }

    Py_DECREF(block);

    return 0;
}


#define SET_ENTRY(type,value) do { \
        PyObject *sub; \
        sub = Py_BuildValue(#type, h->value); \
        PyDict_SetItemString(dict, #value, sub); \
        Py_DECREF(sub); \
    } while (0)

#define SET_BOOL(value) \
    if (h->value) PyDict_SetItemString(dict, #value, Py_True); \
    else PyDict_SetItemString(dict, #value, Py_False)

static PyObject *fill_header(sdf_file_t *h)
{
    PyObject *dict;

    dict = PyDict_New();

    SET_ENTRY(i, file_version);
    SET_ENTRY(i, file_revision);
    SET_ENTRY(s, code_name);
    SET_ENTRY(i, step);
    SET_ENTRY(d, time);
    SET_ENTRY(i, jobid1);
    SET_ENTRY(i, jobid2);
    SET_ENTRY(i, code_io_version);
    SET_BOOL(restart_flag);
    SET_BOOL(other_domains);

    return dict;
}


static void
setup_materials(SDFObject *sdf, PyObject *dict, sdf_block_t *b)
{
    char *block_name = NULL;
    Block *block = NULL;
    PyObject *name = NULL;
    Py_ssize_t i;

    if (!sdf->h || !b) return;

    block_name = b->name;

    block = (Block*)Block_alloc(sdf, b);
    if (!block) goto free_mem;

    if (b->material_names) {
        block->material_names = PyList_New(b->ndims);
        if (!block->material_names) goto free_mem;

        for (i=0; i < b->ndims; i++) {
            name = PyBytes_FromString(b->material_names[i]);
            PyList_SET_ITEM(block->material_names, i, name);
        }
    }

    if (b->variable_ids) {
        block->material_ids = PyList_New(b->ndims);
        if (!block->material_ids) goto free_mem;

        for (i=0; i < b->ndims; i++) {
            name = PyBytes_FromString(b->variable_ids[i]);
            PyList_SET_ITEM(block->material_ids, i, name);
        }
    }

    PyDict_SetItemString(dict, block_name, (PyObject*)block);
    Py_DECREF(block);

    return;

free_mem:
    if (block) Py_DECREF(block);
    return;
}


static void
setup_array(SDFObject *sdf, PyObject *dict, sdf_block_t *b)
{
    char *block_name = NULL;
    Block *block = NULL;

    if (!sdf->h || !b) return;

    block_name = b->name;

    block = (Block*)Block_alloc(sdf, b);
    if (!block) goto free_mem;

    if (b->units) {
        block->units = PyBytes_FromString(b->units);
        if (!block->units) goto free_mem;
    }

    PyDict_SetItemString(dict, block_name, (PyObject*)block);
    Py_DECREF(block);

    return;

free_mem:
    if (block) Py_DECREF(block);
    return;
}


static void
setup_constant(SDFObject *sdf, PyObject *dict, sdf_block_t *b)
{
    Block *block = NULL;
    double dd;
    long il;
    long long ll;
    char cc;

    block = (Block*)Block_alloc(sdf, b);
    if (!block) return;

    switch(b->datatype) {
        case SDF_DATATYPE_REAL4:
            dd = *((float*)b->const_value);
            block->data = PyFloat_FromDouble(dd);
            break;
        case SDF_DATATYPE_REAL8:
            dd = *((double*)b->const_value);
            block->data = PyFloat_FromDouble(dd);
            break;
        case SDF_DATATYPE_INTEGER4:
            il = *((int32_t*)b->const_value);
            block->data = PyLong_FromLong(il);
            break;
        case SDF_DATATYPE_INTEGER8:
            ll = *((int64_t*)b->const_value);
            block->data = PyLong_FromLongLong(ll);
            break;
        case SDF_DATATYPE_LOGICAL:
            cc = *((char*)b->const_value);
            if (cc)
                block->data = Py_True;
            else
                block->data = Py_False;
            break;
    }

    PyDict_SetItemString(dict, b->name, (PyObject*)block);

    Py_DECREF(block);

    return;
}


static void
setup_namevalue(SDFObject *sdf, PyObject *dict, sdf_block_t *b)
{
    Block *block = NULL;
    PyObject *sub;
    float *ff;
    double *dd;
    int32_t *il;
    int64_t *ll;
    char *cc;
    Py_ssize_t i;

    block = (Block*)Block_alloc(sdf, b);
    if (!block) return;

    block->dict = PyDict_New();
    if (!block->dict) goto free_mem;

    switch(b->datatype) {
        case SDF_DATATYPE_REAL4:
            ff = (float*)b->data;
            for (i=0; i < b->ndims; i++) {
                sub = PyFloat_FromDouble(*ff);
                PyDict_SetItemString(block->dict, b->material_names[i], sub);
                Py_DECREF(sub);
                ff++;
            }
            break;
        case SDF_DATATYPE_REAL8:
            dd = (double*)b->data;
            for (i=0; i < b->ndims; i++) {
                sub = PyFloat_FromDouble(*dd);
                PyDict_SetItemString(block->dict, b->material_names[i], sub);
                Py_DECREF(sub);
                dd++;
            }
            break;
        case SDF_DATATYPE_INTEGER4:
            il = (int32_t*)b->data;
            for (i=0; i < b->ndims; i++) {
                sub = PyLong_FromLong(*il);
                PyDict_SetItemString(block->dict, b->material_names[i], sub);
                Py_DECREF(sub);
                il++;
            }
            break;
        case SDF_DATATYPE_INTEGER8:
            ll = (int64_t*)b->data;
            for (i=0; i < b->ndims; i++) {
                sub = PyLong_FromLongLong(*ll);
                PyDict_SetItemString(block->dict, b->material_names[i], sub);
                Py_DECREF(sub);
                ll++;
            }
            break;
        case SDF_DATATYPE_LOGICAL:
            cc = (char*)b->data;
            for (i=0; i < b->ndims; i++) {
                if (*cc)
                    sub = Py_True;
                else
                    sub = Py_False;
                PyDict_SetItemString(block->dict, b->material_names[i], sub);
                Py_DECREF(sub);
                cc++;
            }
            break;
    }

    PyDict_SetItemString(dict, b->name, (PyObject*)block);

    Py_DECREF(block);

    return;

free_mem:
    if (block) Py_DECREF(block);
    return;
}


static Block *dict_find_mesh_id(PyObject *dict, char *id)
{
    PyObject *value;
    Block *block;

    value = PyDict_GetItemString(dict, id);
    if ( !value )
       return NULL;

    if ( !PyObject_TypeCheck(value, &BlockType) )
       return NULL;

    block = (Block*)value;

    switch(block->b->blocktype) {
        case SDF_BLOCKTYPE_PLAIN_MESH:
        case SDF_BLOCKTYPE_POINT_MESH:
        case SDF_BLOCKTYPE_LAGRANGIAN_MESH:
           return block;
    }

    return NULL;
}


static void dict_find_variable_ids(PyObject *dict, Block *station)
{
    PyObject *key, *value;
    Block *block;
    char *block_id;
    Py_ssize_t pos = 0, len, i, *find, nfind, found;
    sdf_block_t *b;

    b = station->b;
    if (!station->b)
        return;

    nfind = b->ndims;
    find = malloc(nfind * sizeof(Py_ssize_t));
    for (i=0; i < nfind; i++)
        find[i] = i;

    while (PyDict_Next(dict, &pos, &key, &value)) {
        if (!PyObject_TypeCheck(value, &BlockType))
            continue;
        block = (Block*)value;
        if (!block->b)
            continue;
        block_id = PyBytes_AsString(block->id);
        if (!block_id)
            continue;
        len = strlen(block_id) + 1;

        // Loop over remaining variable_ids and check for a match
        found = -1;
        for (i=0; i < nfind; i++) {
            found = find[i];
            if (memcmp(block_id, b->variable_ids[found], len) == 0)
                break;
            found = -1;
        }

        if (found < 0) continue;

        if (!station->data) {
            station->data = PyList_New(b->ndims);
            if (!station->data) return;
        }

        // Found one of our variable_ids. Insert it into the station list and
        // remove it from the find list.
        PyList_SET_ITEM(station->data, found, (PyObject*)block);
        Py_INCREF(block);

        nfind--;
        // Stop searching if we've found them all
        if (nfind <= 0) break;

        for (i=found; i < nfind; i++)
            find[i] = find[i+1];
    }

    free(find);

    return;
}


static PyObject* SDF_read(PyObject *self, PyObject *args, PyObject *kw)
{
    SDFObject *sdf;
    PyTypeObject *type = &SDFType;
    sdf_file_t *h;
    sdf_block_t *b;
    PyObject *dict, *dict_id, *sub, *key, *value;
    PyObject *items_list;
    Block *block;
    Py_ssize_t pos = 0;
    int i, convert, use_mmap, use_dict, use_derived, mode, len_id;
    int mangled;
    comm_t comm;
    const char *file;
    char *mesh_id;
    static char *kwlist[] = {"file", "convert", "mmap", "dict", "derived",
        "stations", "variables", "t0", "t1", NULL};
    PyObject *stations = NULL, *variables = NULL;
    double t0 = -DBL_MAX, t1 = DBL_MAX;
    BlockList *blocklist;

    convert = 0; use_mmap = 1; use_dict = 0; use_derived = 1;
    mode = SDF_READ; comm = 0;

    if (!PyArg_ParseTupleAndKeywords(args, kw, "s|iiiiO!O!dd", kwlist, &file,
            &convert, &use_mmap, &use_dict, &use_derived, &PyList_Type,
            &stations, &PyList_Type, &variables, &t0, &t1))
        return NULL;

    sdf = (SDFObject*)type->tp_alloc(type, 0);
    if (!sdf) {
        PyErr_Format(PyExc_MemoryError, "Failed to allocate SDF object");
        return NULL;
    }

    h = sdf_open(file, comm, mode, use_mmap);
    sdf->h = h;
    if (!sdf->h) {
        PyErr_Format(PyExc_IOError, "Failed to open file: '%s'", file);
        Py_DECREF(sdf);
        return NULL;
    }

    sdf_stack_init(h);

    if (convert) h->use_float = 1;

    if (use_derived)
        sdf_read_blocklist_all(h);
    else
        sdf_read_blocklist(h);

    dict = PyDict_New();
    dict_id = PyDict_New();

    /* Add header */
    sub = fill_header(h);
    PyDict_SetItemString(dict, "Header", sub);
    Py_DECREF(sub);

    b = h->current_block = h->blocklist;
    for (i = 0; i < h->nblocks; i++) {
        switch(b->blocktype) {
            case SDF_BLOCKTYPE_PLAIN_MESH:
            case SDF_BLOCKTYPE_POINT_MESH:
            case SDF_BLOCKTYPE_LAGRANGIAN_MESH:
                setup_mesh(sdf, dict, b);
                break;
            case SDF_BLOCKTYPE_PLAIN_VARIABLE:
            case SDF_BLOCKTYPE_PLAIN_DERIVED:
            case SDF_BLOCKTYPE_POINT_VARIABLE:
            case SDF_BLOCKTYPE_POINT_DERIVED:
            case SDF_BLOCKTYPE_ARRAY:
                setup_array(sdf, dict, b);
                break;
            case SDF_BLOCKTYPE_CONSTANT:
                setup_constant(sdf, dict, b);
                break;
            case SDF_BLOCKTYPE_NAMEVALUE:
                setup_namevalue(sdf, dict, b);
                break;
            case SDF_BLOCKTYPE_STATION:
                sub = PyDict_GetItemString(dict, "StationBlocks");
                if ( !sub ) {
                    sub = PyDict_New();
                    PyDict_SetItemString(dict, "StationBlocks", sub);
                    Py_DECREF(sub);
                }
                append_station_metadata(b, sub);
                extract_station_time_histories(h, stations, variables, t0, t1,
                        dict);
                break;
            case SDF_BLOCKTYPE_STITCHED_MATERIAL:
            case SDF_BLOCKTYPE_CONTIGUOUS_MATERIAL:
                setup_materials(sdf, dict, b);
                break;
        }
        sub = PyDict_GetItemString(dict, b->name);
        if ( sub )
            PyDict_SetItemString(dict_id, b->id, sub);
        b = h->current_block = b->next;
    }

    len_id = h->string_length + 5;
    mesh_id = malloc(len_id);

    while (PyDict_Next(dict, &pos, &key, &value)) {
        if (!PyObject_TypeCheck(value, &BlockType))
            continue;
        block = (Block*)value;
        b = block->b;
        if (!b) continue;
        if (b->blocktype == SDF_BLOCKTYPE_PLAIN_VARIABLE
                || b->blocktype == SDF_BLOCKTYPE_PLAIN_DERIVED
                || b->blocktype == SDF_BLOCKTYPE_STITCHED_MATERIAL
                || b->blocktype == SDF_BLOCKTYPE_CONTIGUOUS_MATERIAL) {
            if (!b->mesh_id)
                continue;
            block->grid = dict_find_mesh_id(dict_id, b->mesh_id);
            len_id = strlen(b->mesh_id);
            memcpy(mesh_id, b->mesh_id, len_id);
            memcpy(mesh_id+len_id, "_mid", 5);
            block->grid_mid = dict_find_mesh_id(dict_id, mesh_id);
            if (b->blocktype == SDF_BLOCKTYPE_STITCHED_MATERIAL
                    || b->blocktype == SDF_BLOCKTYPE_CONTIGUOUS_MATERIAL)
                dict_find_variable_ids(dict, block);
        } else if (b->blocktype == SDF_BLOCKTYPE_POINT_VARIABLE
                || b->blocktype == SDF_BLOCKTYPE_POINT_DERIVED) {
            block->grid = dict_find_mesh_id(dict_id, b->mesh_id);
        }
    }

    free(mesh_id);

    if (use_dict) {
        Py_DECREF(dict_id);
        Py_DECREF(sdf);
        return (PyObject*)dict;
    }

    type = &BlockListType;
    blocklist = (BlockList*)type->tp_alloc(type, 0);
    if (!blocklist) {
        Py_DECREF(dict_id);
        PyErr_Format(PyExc_MemoryError, "Failed to allocate BlockList object");
        return NULL;
    }
    blocklist->dict = dict;

    /* Mangle dictionary names */
    items_list = PyDict_Items(dict);
    for (i = 0; i < PyList_GET_SIZE(items_list); i++) {
        PyObject *item = PyList_GET_ITEM(items_list, i);
        PyObject *key = PyTuple_GET_ITEM(item, 0);
        PyObject *value = PyTuple_GET_ITEM(item, 1);
        char *ckey, *ptr;

        mangled = 0;

#if PY_MAJOR_VERSION < 3
        ckey = strdup(PyBytes_AsString(key));
#else
        ckey = strdup(PyBytes_AsString(PyUnicode_AsASCIIString(key)));
#endif

        for (ptr = ckey; *ptr != '\0'; ptr++) {
            if (*ptr >= '0' && *ptr <= '9')
                continue;
            if (*ptr >= 'A' && *ptr <= 'Z')
                continue;
            if (*ptr >= 'a' && *ptr <= 'z')
                continue;
            *ptr = '_';
            mangled = 1;
        }

        if (mangled) {
            PyDict_DelItem(dict, key);
            PyDict_SetItemString(dict, ckey, value);
        }

        free(ckey);
    }
    Py_DECREF(items_list);

    Py_DECREF(dict_id);
    Py_DECREF(sdf);
    return (PyObject*)blocklist;
}


static PyMethodDef SDF_methods[] = {
    {"read", (PyCFunction)SDF_read, METH_VARARGS | METH_KEYWORDS,
     "read(file, [convert, mmap, dict, derived, stations, variables, t0, t1])\n"
     "\nReads the SDF data and returns a dictionary of NumPy arrays.\n\n"
     "Parameters\n"
     "----------\n"
     "file : string\n"
     "    The name of the SDF file to open.\n"
     "convert : bool, optional\n"
     "    Convert double precision data to single when reading file.\n"
     "mmap : bool, optional\n"
     "    Use mmap to map file contents into memory.\n"
     "dict : bool, optional\n"
     "    Return file contents as a dictionary rather than member names.\n"
     "derived : bool, optional\n"
     "    Include derived variables in the data structure.\n"
     "stations : string list, optional\n"
     "    List of stations to read.\n"
     "variables : string list, optional\n"
     "    List of station variables to read.\n"
     "t0 : double, optional\n"
     "    Starting time for station data.\n"
     "t1 : double, optional\n"
     "    Ending time for station data.\n"
     },
    {NULL}
};

#define ADD_TYPE(name,base) do { \
        name##Type = base; \
        name##Type.tp_name = "sdf." #name; \
        if (PyType_Ready(&name##Type) < 0) \
            return MOD_ERROR_VAL; \
        Py_INCREF(&name##Type); \
        if (PyModule_AddObject(m, #name, (PyObject *)&name##Type) < 0) \
            return MOD_ERROR_VAL; \
    } while(0)


MOD_INIT(sdf)
{
    PyObject *m;
    char *s;

    MOD_DEF(m, "sdf", "SDF file reading library", SDF_methods)

    if (!m)
        return MOD_ERROR_VAL;

    PyModule_AddStringConstant(m, "__version__", "2.4.0");
    PyModule_AddStringConstant(m, "__commit_id__", SDF_COMMIT_ID);
    PyModule_AddStringConstant(m, "__commit_date__", SDF_COMMIT_DATE);
    s = sdf_get_library_commit_id();
    PyModule_AddStringConstant(m, "__library_commit_id__", s);
    if (s)
       free(s);
    s = sdf_get_library_commit_date();
    PyModule_AddStringConstant(m, "__library_commit_date__", s);
    if (s)
       free(s);

    SDFType.tp_dealloc = SDF_dealloc;
    SDFType.tp_flags = Py_TPFLAGS_DEFAULT;
    SDFType.tp_doc = "SDF constructor accepts two arguments.\n"
        "The first is the SDF filename to open. This argument is mandatory.\n"
        "The second argument is an optional integer. If it is non-zero then "
        "the\ndata is converted from double precision to single.";
    SDFType.tp_methods = SDF_methods;
    if (PyType_Ready(&SDFType) < 0)
        return MOD_ERROR_VAL;

    ArrayType.tp_dealloc = Array_dealloc;
    ArrayType.tp_flags = Py_TPFLAGS_DEFAULT;
    if (PyType_Ready(&ArrayType) < 0)
        return MOD_ERROR_VAL;

    BlockListType.tp_dealloc = BlockList_dealloc;
    BlockListType.tp_flags = Py_TPFLAGS_DEFAULT;
    BlockListType.tp_dictoffset = offsetof(BlockList, dict);
    BlockListType.tp_members = BlockList_members;
    if (PyType_Ready(&BlockListType) < 0)
        return MOD_ERROR_VAL;
    if (PyModule_AddObject(m, "BlockList", (PyObject *)&BlockListType) < 0)
        return MOD_ERROR_VAL;

    BlockType.tp_flags = Py_TPFLAGS_DEFAULT;
    BlockType.tp_doc = "SDF block type.\n"
        "Contains the data and metadata for a single "
        "block from an SDF file.";
    BlockBase = BlockType;
    BlockBase.tp_base = &BlockType;

    BlockType.tp_name = "sdf.Block";
    BlockType.tp_dealloc = Block_dealloc;
    BlockType.tp_members = Block_members;
    if (PyType_Ready(&BlockType) < 0)
        return MOD_ERROR_VAL;
    Py_INCREF(&BlockType);
    if (PyModule_AddObject(m, "Block", (PyObject *)&BlockType) < 0)
        return MOD_ERROR_VAL;

    ADD_TYPE(BlockConstant, BlockBase);
    ADD_TYPE(BlockStation, BlockBase);

    BlockBase.tp_getset = Block_getset;
    ADD_TYPE(BlockArray, BlockBase);

    BlockBase.tp_getset = 0;
    BlockBase.tp_dictoffset = offsetof(Block, dict);
    ADD_TYPE(BlockNameValue, BlockBase);

    BlockBase.tp_dictoffset = 0;
    BlockBase.tp_members = BlockMesh_members;
    ADD_TYPE(BlockMesh, BlockBase);

    BlockBase.tp_members = BlockStitchedMaterial_members;
    ADD_TYPE(BlockStitchedMaterial, BlockBase);

    BlockBase.tp_base = &BlockArrayType;
    BlockBase.tp_getset = Block_getset;
    BlockBase.tp_members = BlockMeshVariable_members;

    ADD_TYPE(BlockPlainVariable, BlockBase);

    BlockBase.tp_members = BlockPointVariable_members;
    ADD_TYPE(BlockPointVariable, BlockBase);

    BlockBase.tp_base = &BlockMeshType;
    BlockBase.tp_members = BlockPlainMesh_members;
    ADD_TYPE(BlockPlainMesh, BlockBase);
    ADD_TYPE(BlockLagrangianMesh, BlockBase);

    BlockBase.tp_members = BlockPointMesh_members;
    ADD_TYPE(BlockPointMesh, BlockBase);

    import_array();   /* required NumPy initialization */

    return MOD_SUCCESS_VAL(m);
}
