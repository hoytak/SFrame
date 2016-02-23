
from libcpp.string cimport string
from cpython.version cimport PY_MAJOR_VERSION

cdef string _attempt_cast_str_to_cpp(py_s) except *:
    """
    The last resort conversion routine for strings cast 
    """

    cdef bint success

    # Try this version first, as casting something to bytes is much
    # restrictive and tends to succeed only if it's a legit cast.
    # However, the cast to str can succeed even if it's actually a
    # bytes class -- e.g. np.string_.
    if PY_MAJOR_VERSION >= 3:
    
        try:
            py_s = bytes(py_s)
            success = True
        except:
            success = False

        if success:
            return (<bytes>py_s)

    # Some classes (e.g. np.string_) will succeed with a string
    # representation of the class, e.g. np.string_('a') == b'a', but
    # str(np.string_('a')) == "b'a'".  Thus we need to ensure it's
    # actually a legit representation as well.
    try:
        new_py_s = str(py_s)
        success = (new_py_s == py_s)
        py_s = new_py_s
    except:
        success = False

    if success:
        return unsafe_str_to_cpp(py_s)

    # Now,see about the unicode route. 
    if PY_MAJOR_VERSION == 2:
        try:
            new_py_s = unicode(py_s)
            success = (new_py_s == py_s)
            py_s = new_py_s
        except:
            success = False

        if success:
            return unsafe_unicode_to_cpp(py_s)

    # Okay, none of these worked, so error out.
    raise TypeError("Type '%s' cannot be interpreted as str." % str(type(py_s)))


def str_to_char_p_castable(py_s):
    """
    Use this function to convert any string-like object to something that can be converted to a 
    """
    cdef type t = type(py_s)
    
    if PY_MAJOR_VERSION >= 3:
        if t is str:
            return (<str>py_s).encode()
        elif t is bytes:
            return (<bytes>py_s)
        else:
            return bytes(py_s)
    else:
        if t is str:
            return (<str>py_s)
        elif t is unicode:
            return (<unicode>py_s).encode('UTF-8')
        else:
            return str(py_s)
