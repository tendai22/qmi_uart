#pragma once
//
// protothread.h ... pseudo-coroutine macros
//

#define PT_BEGIN(pt) \
    switch((pt)->line) \
    { \
    case 0:

#define PT_END(pt) \
    } \
    (pt)->line = 0; \
    return

// PT_WAIT_UNTIL ... while(!cond);
// condが成立するまでここで待ち続ける
#define PT_WAIT_UNTIL_L(pt, cond, label) \
    do { \
        case __LINE__: \
        pt->line = __LINE__; \
        if (!(cond)) \
            return; \
    } while (0)
