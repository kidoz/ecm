#ifndef ECM_UNECM_H
#define ECM_UNECM_H

#if defined(WIN32) || defined(WIN64) || defined(_WIN32) || defined(_WIN64)
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

#endif //ECM_UNECM_H
