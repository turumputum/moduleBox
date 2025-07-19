// ***************************************************************************
// TITLE
//
// PROJECT
//
// ***************************************************************************
//
// FILE
//      $Id$
// HISTORY
//      $Log$
// ***************************************************************************

#ifndef __LITE_ARSENAL_H__
#define __LITE_ARSENAL_H__


#define MAC_MIN(a,b)            ((a) < (b) ? (a) : (b))
#define MAC_MAX(a,b)            ((a) > (b) ? (a) : (b))

#define nil                     0

#define AXII                    ((UINT)(-1))
#define AXIP                    ((PCVOID)(-1))


#define AXPACKED(a) a __attribute__((aligned(1),packed))

typedef char                    I8;
typedef unsigned short          U16;
typedef long long               I64;
typedef unsigned long long      U64;
typedef int                     I32;
typedef unsigned int            U32;
typedef unsigned char           U8;
typedef short                   I16;
typedef unsigned int            UINT;
typedef int                     INT;
typedef unsigned char *         PU8;
typedef unsigned short *        PU16;
typedef unsigned long long *    PU64;
typedef long long *             PI64;
typedef char *                  PSTR;
typedef const char *            PCSTR;

typedef unsigned long           ULONG;
typedef long                    LONG;

typedef void *                  PVOID;

typedef int                     BOOL;

#if !defined(true)
#define true                    (BOOL)1
#define false                   (BOOL)0
#endif

#define ENTER(a)                
#define QUIT                                
#define RETURN(a)               return (a)


#endif // #ifndef __LITE_ARSENAL_H__
