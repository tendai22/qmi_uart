#pragma once
//
// protothread.h ... pseudo-coroutine macros
//

// PT_BEGIN
#define PT_BEGIN()     \
    static int pt_line = 0; \
    switch (pt_line) { \
    case 0:

// PT_END

#define PT_END() \
    }            \
    pt_line = 0; \
    return true

// PT_WAIT_UNTIL ... while(!cond);
// condが成立するまでここで待ち続ける
#define PT_WAIT_UNTIL(cond) \
    do {                    \
        pt_line = __LINE__;    \
    case __LINE__:          \
        if (!(cond))        \
            return false;   \
    } while (0)

