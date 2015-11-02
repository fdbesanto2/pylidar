/*
 * riegl.cpp
 *
 *
 * This file is part of PyLidar
 * Copyright (C) 2015 John Armston, Pete Bunting, Neil Flood, Sam Gillingham
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <Python.h>
#include "numpy/arrayobject.h"
#include "pylidar.h"

#include <riegl/scanlib.hpp>
#include <cmath>

/* An exception object for this module */
/* created in the init function */
struct RieglState
{
    PyObject *error;
};

#if PY_MAJOR_VERSION >= 3
#define GETSTATE(m) ((struct RieglState*)PyModule_GetState(m))
#else
#define GETSTATE(m) (&_state)
static struct RieglState _state;
#endif

/* Python object wrapping a scanlib::basic_rconnection */
typedef struct
{
    PyObject_HEAD
    std::shared_ptr<scanlib::basic_rconnection> rc;
    scanlib::decoder_rxpmarker *pDecoder;
    scanlib::buffer *pBuffer;
    Py_ssize_t nTotalPulsesRead;
    bool bFinishedReading;
} PyRieglScanFile;

#if PY_MAJOR_VERSION >= 3
static int riegl_traverse(PyObject *m, visitproc visit, void *arg) 
{
    Py_VISIT(GETSTATE(m)->error);
    return 0;
}

static int riegl_clear(PyObject *m) 
{
    Py_CLEAR(GETSTATE(m)->error);
    return 0;
}

static struct PyModuleDef moduledef = {
        PyModuleDef_HEAD_INIT,
        "_riegl",
        NULL,
        sizeof(struct RieglState),
        NULL,
        NULL,
        riegl_traverse,
        riegl_clear,
        NULL
};
#endif

/* destructor - close and delete tc */
static void 
PyRieglScanFile_dealloc(PyRieglScanFile *self)
{
    self->rc->close();
    self->rc.reset();
    delete self->pDecoder;
    delete self->pBuffer;
    Py_TYPE(self)->tp_free((PyObject*)self);
}

/* init method - open file */
static int 
PyRieglScanFile_init(PyRieglScanFile *self, PyObject *args, PyObject *kwds)
{
char *pszFname = NULL;

    if( !PyArg_ParseTuple(args, "s", &pszFname ) )
    {
        return -1;
    }

    try
    {
        self->rc = scanlib::basic_rconnection::create(pszFname);

        // The decoder class scans off distinct packets from the continuous data stream
        // i.e. the rxp format and manages the packets in a buffer.
        self->pDecoder = new scanlib::decoder_rxpmarker(self->rc);

        // The buffer is a structure that holds pointers into the decoder buffer
        // thereby avoiding unnecessary copies of the data.
        self->pBuffer = new scanlib::buffer();
    }
    catch(scanlib::scanlib_exception e)
    {
        // raise Python exception
        PyObject *m;
#if PY_MAJOR_VERSION >= 3
        // best way I could find for obtaining module reference
        // from inside a class method. Not needed for Python < 3.
        m = PyState_FindModule(&moduledef);
#endif
        PyErr_Format(GETSTATE(m)->error, "Error from Riegl lib: %s", e.what());
        return -1;
    }   
    self->nTotalPulsesRead = 0;
    self->bFinishedReading = false;
    return 0;
}

/* Structure for pulses */
typedef struct {
    uint64_t pulseID;
    uint64_t gpsTime;
    float azimuth;
    float zenith;
    uint32_t scanline;
    uint16_t scanlineIdx;
    double xIdx;
    double yIdx;
    double xOrigin;
    double yOrigin;
    float zOrigin;
    uint32_t pointStartIdx;
    uint16_t pointCount;
} SRieglPulse;

/* field info for pylidar_structArrayToNumpy */
static SpylidarFieldDefn RieglPulseFields[] = {
    CREATE_FIELD_DEFN(SRieglPulse, pulseID, 'u'),
    CREATE_FIELD_DEFN(SRieglPulse, gpsTime, 'u'),
    CREATE_FIELD_DEFN(SRieglPulse, azimuth, 'f'),
    CREATE_FIELD_DEFN(SRieglPulse, zenith, 'f'),
    CREATE_FIELD_DEFN(SRieglPulse, scanline, 'u'),
    CREATE_FIELD_DEFN(SRieglPulse, scanlineIdx, 'u'),
    CREATE_FIELD_DEFN(SRieglPulse, yIdx, 'f'),
    CREATE_FIELD_DEFN(SRieglPulse, xIdx, 'f'),
    CREATE_FIELD_DEFN(SRieglPulse, xOrigin, 'f'),
    CREATE_FIELD_DEFN(SRieglPulse, yOrigin, 'f'),
    CREATE_FIELD_DEFN(SRieglPulse, zOrigin, 'f'),
    CREATE_FIELD_DEFN(SRieglPulse, pointStartIdx, 'u'),
    CREATE_FIELD_DEFN(SRieglPulse, pointCount, 'u'),
    {NULL} // Sentinel
};

/* Structure for points */
typedef struct {
    uint64_t returnId;
    uint64_t gpsTime;
    float amplitudeReturn;
    float widthReturn;
    uint8_t classification;
    double x;
    double y;
    float z;
} SRieglPoint;

/* field info for pylidar_structArrayToNumpy */
static SpylidarFieldDefn RieglPointFields[] = {
    CREATE_FIELD_DEFN(SRieglPoint, returnId, 'u'),
    CREATE_FIELD_DEFN(SRieglPoint, gpsTime, 'u'),
    CREATE_FIELD_DEFN(SRieglPoint, amplitudeReturn, 'f'),
    CREATE_FIELD_DEFN(SRieglPoint, widthReturn, 'f'),
    CREATE_FIELD_DEFN(SRieglPoint, classification, 'u'),
    CREATE_FIELD_DEFN(SRieglPoint, x, 'f'),
    CREATE_FIELD_DEFN(SRieglPoint, y, 'f'),
    CREATE_FIELD_DEFN(SRieglPoint, z, 'f'),
    {NULL} // Sentinel
};

class RieglReader : public scanlib::pointcloud
{
public:
    RieglReader(Py_ssize_t nPulses, Py_ssize_t nTotalPulsesRead) : 
        scanlib::pointcloud(false), 
        m_nPulses(nPulses),
        m_nTotalPulsesRead(nTotalPulsesRead),
        m_scanline(0),
        m_scanlineIdx(0),
        m_Pulses(nPulses, 100),
        m_Points(nPulses, 100)
    {
    }

    bool done()
    {
        return m_Pulses.getNumElems() >= m_nPulses;
    }

    Py_ssize_t getNumPulsesRead()
    {
        return m_Pulses.getNumElems();
    }

    PyObject *getPulses()
    {
        return m_Pulses.getNumpyArray(RieglPulseFields);
    }

    PyObject *getPoints()
    {
        return m_Points.getNumpyArray(RieglPointFields);
    }

protected:
    // This call is invoked for every pulse, even if there is no return
    void on_shot()
    {
        if( done() )
        {
            fprintf(stderr, "Have got all pulses, but on_shot() still called\n");
            return;
        }
        m_scanlineIdx++;

        SRieglPulse pulse;
        pulse.pulseID = m_Pulses.getNumElems() + m_nTotalPulsesRead;
        pulse.gpsTime = time_sorg * 1e9 + 0.5;

        // Get spherical coordinates. TODO: matrix transform
        double magnitude = std::sqrt(beam_direction[0] * beam_direction[0] + \
                           beam_direction[1] * beam_direction[1] + \
                           beam_direction[2] * beam_direction[2]);
        double shot_zenith = std::acos(beam_direction[2]/magnitude) * 180.0 / pi;
        double shot_azimuth = shot_azimuth = std::atan2(beam_direction[0],beam_direction[1]) * 180.0 / pi;      
        if( beam_direction[0] < 0 )
        {
            shot_azimuth += 360.0;            
        }

        pulse.azimuth = shot_azimuth;
        pulse.zenith = shot_zenith;
        pulse.scanline = m_scanline;
        pulse.scanlineIdx = m_scanlineIdx;
        // do we need these separate?
        pulse.xIdx = m_scanline;
        pulse.yIdx = m_scanlineIdx;
        // TODO: matrix transform
        pulse.xOrigin = beam_origin[0];
        pulse.yOrigin = beam_origin[1];
        pulse.zOrigin = beam_origin[2];
        // TODO: pointStartIdx etc

        m_Pulses.push(&pulse);
    }

    // start of a scan line going in the up direction
    void on_line_start_up(const scanlib::line_start_up<iterator_type>& arg) 
    {
        scanlib::pointcloud::on_line_start_up(arg);
        ++m_scanline;
        m_scanlineIdx = 0;
    }
    
    // start of a scan line going in the down direction
    void on_line_start_dn(const scanlib::line_start_dn<iterator_type>& arg) 
    {
        scanlib::pointcloud::on_line_start_dn(arg);
        ++m_scanline;
        m_scanlineIdx = 0;
    }

private:
    Py_ssize_t m_nPulses;
    Py_ssize_t m_nTotalPulsesRead;
    PylidarVector<SRieglPulse> m_Pulses;
    PylidarVector<SRieglPoint> m_Points;
    uint32_t m_scanline;
    uint16_t m_scanlineIdx;
};

static PyObject *PyRieglScanFile_readData(PyRieglScanFile *self, PyObject *args)
{
    Py_ssize_t nPulseStart, nPulseEnd, nPulses, nCount;
    if( !PyArg_ParseTuple(args, "nn:readData", &nPulseStart, &nPulseEnd ) )
        return NULL;

    nPulses = nPulseEnd - nPulseStart;

    // our reader class
    RieglReader reader(nPulses, self->nTotalPulsesRead);

    // loop through the requested number of pulses
    try
    {
        for( self->pDecoder->get(*self->pBuffer); 
                !self->pDecoder->eoi() && !reader.done(); self->pDecoder->get(*self->pBuffer) )
        {
            //fprintf(stderr, "starting loop %ld %ld\n", reader.getNumPulsesRead(), self->rc->tellg());
            reader.dispatch(self->pBuffer->begin(), self->pBuffer->end());
            //fprintf(stderr, "ending loop %ld %ld\n", reader.getNumPulsesRead(), self->rc->tellg());
        }
    }
    catch(scanlib::scanlib_exception e)
    {
        // raise Python exception
        PyObject *m;
#if PY_MAJOR_VERSION >= 3
        // best way I could find for obtaining module reference
        // from inside a class method. Not needed for Python < 3.
        m = PyState_FindModule(&moduledef);
#endif
        PyErr_Format(GETSTATE(m)->error, "Error from Riegl lib: %s", e.what());
        return NULL;
    }

    // update how many were actually read
    self->nTotalPulsesRead += reader.getNumPulsesRead();

    // we have finished if we are at the end
    self->bFinishedReading = self->pDecoder->eoi();

    // get pulse array as numpy array
    PyObject *pPulses = reader.getPulses(); 

    // build tuple
    PyObject *pTuple = PyTuple_New(2);
    PyTuple_SetItem(pTuple, 0, pPulses);
    Py_INCREF(Py_None);
    PyTuple_SetItem(pTuple, 1, Py_None);

    return pTuple;
}

/* Table of methods */
static PyMethodDef PyRieglScanFile_methods[] = {
    {"readData", (PyCFunction)PyRieglScanFile_readData, METH_VARARGS, NULL},
    {NULL}  /* Sentinel */
};

static PyObject *PyRieglScanFile_getFinished(PyRieglScanFile *self, void *closure)
{
    if( self->bFinishedReading )
        Py_RETURN_TRUE;
    else
        Py_RETURN_FALSE;
}

static PyObject *PyRieglScanFile_getPulsesRead(PyRieglScanFile *self, void *closure)
{
    return PyLong_FromSsize_t(self->nTotalPulsesRead);
}

/* get/set */
static PyGetSetDef PyRieglScanFile_getseters[] = {
    {"finished", (getter)PyRieglScanFile_getFinished, NULL, "Get Finished reading state", NULL}, 
    {"pulsesRead", (getter)PyRieglScanFile_getPulsesRead, NULL, "Get number of pulses read", NULL},
    {NULL}  /* Sentinel */
};

static PyTypeObject PyRieglScanFileType = {
#if PY_MAJOR_VERSION >= 3
    PyVarObject_HEAD_INIT(NULL, 0)
#else
    PyObject_HEAD_INIT(NULL)
    0,                         /*ob_size*/
#endif
    "_riegl.ScanFile",         /*tp_name*/
    sizeof(PyRieglScanFile),   /*tp_basicsize*/
    0,                         /*tp_itemsize*/
    (destructor)PyRieglScanFile_dealloc, /*tp_dealloc*/
    0,                         /*tp_print*/
    0,                         /*tp_getattr*/
    0,                         /*tp_setattr*/
    0,                         /*tp_compare*/
    0,                         /*tp_repr*/
    0,                         /*tp_as_number*/
    0,                         /*tp_as_sequence*/
    0,                         /*tp_as_mapping*/
    0,                         /*tp_hash */
    0,                         /*tp_call*/
    0,                         /*tp_str*/
    0,                         /*tp_getattro*/
    0,                         /*tp_setattro*/
    0,                         /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /*tp_flags*/
    "Riegl Scan File object",           /* tp_doc */
    0,                     /* tp_traverse */
    0,                     /* tp_clear */
    0,                     /* tp_richcompare */
    0,                     /* tp_weaklistoffset */
    0,                     /* tp_iter */
    0,                     /* tp_iternext */
    PyRieglScanFile_methods,             /* tp_methods */
    0,             /* tp_members */
    PyRieglScanFile_getseters,           /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)PyRieglScanFile_init,      /* tp_init */
    0,                         /* tp_alloc */
    0,                 /* tp_new */
};
#if PY_MAJOR_VERSION >= 3

#define INITERROR return NULL

PyMODINIT_FUNC 
PyInit__riegl(void)

#else
#define INITERROR return

PyMODINIT_FUNC
init_riegl(void)
#endif
{
    PyObject *pModule;
    struct RieglState *state;

    /* initialize the numpy stuff */
    import_array();
    /* same for pylidar functions */
    pylidar_init();

#if PY_MAJOR_VERSION >= 3
    pModule = PyModule_Create(&moduledef);
#else
    pModule = Py_InitModule("_riegl", RieglMethods);
#endif
    if( pModule == NULL )
        INITERROR;

    state = GETSTATE(pModule);

    /* Create and add our exception type */
    state->error = PyErr_NewException("_riegl.error", NULL, NULL);
    if( state->error == NULL )
    {
        Py_DECREF(pModule);
        INITERROR;
    }

    /* Scan file type */
    PyRieglScanFileType.tp_new = PyType_GenericNew;
    if( PyType_Ready(&PyRieglScanFileType) < 0 )
#if PY_MAJOR_VERSION >= 3
        return NULL;
#else
        return;
#endif

    Py_INCREF(&PyRieglScanFileType);
    PyModule_AddObject(pModule, "ScanFile", (PyObject *)&PyRieglScanFileType);

#if PY_MAJOR_VERSION >= 3
    return pModule;
#endif
}