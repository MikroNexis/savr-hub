#ifndef _APP_VERSION_H_
#define _APP_VERSION_H_

/* The template values come from cmake/modules/version.cmake
 * BUILD_VERSION related template values will be 'git describe',
 * alternatively user defined BUILD_VERSION.
 */

/* #undef ZEPHYR_VERSION_CODE */
/* #undef ZEPHYR_VERSION */

#define APPVERSION                   0x3040100
#define APP_VERSION_NUMBER           0x30401
#define APP_VERSION_MAJOR            3
#define APP_VERSION_MINOR            4
#define APP_PATCHLEVEL               1
#define APP_TWEAK                    0
#define APP_EXTRAVERSION             
#define APP_VERSION_STRING           "3.4.1"
#define APP_VERSION_EXTENDED_STRING  "3.4.1+0"
#define APP_VERSION_TWEAK_STRING     "3.4.1+0"

#define APP_BUILD_VERSION bc0b28b7a4bf


#endif /* _APP_VERSION_H_ */
