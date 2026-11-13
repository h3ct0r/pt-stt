/* The miniaudio implementation, compiled once. Only capture and decoding are used. */
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_GENERATION
#define MA_NO_ENCODING
#include "miniaudio.h"
