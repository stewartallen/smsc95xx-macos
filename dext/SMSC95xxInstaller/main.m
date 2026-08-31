/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal host app for SMSC95xxDriver.dext. A DriverKit extension can only be
 * installed by an app submitting an OSSystemExtensionRequest, so this exists
 * purely to carry the dext and submit that request.
 *
 *   SMSC95xxInstaller activate      install / replace the dext
 *   SMSC95xxInstaller deactivate    remove it
 */

#include <string.h>

#import <Foundation/Foundation.h>
#import <SystemExtensions/SystemExtensions.h>

static NSString * const kDextIdentifier = @"com.github.stewartallen.smsc95xx.driver";

@interface InstallerDelegate : NSObject <OSSystemExtensionRequestDelegate>
@end

@implementation InstallerDelegate

- (OSSystemExtensionReplacementAction)request:(OSSystemExtensionRequest *)request
                  actionForReplacingExtension:(OSSystemExtensionProperties *)existing
                                withExtension:(OSSystemExtensionProperties *)replacement
{
    fprintf(stderr, "replacing %s with %s\n",
            existing.bundleShortVersion.UTF8String,
            replacement.bundleShortVersion.UTF8String);
    return OSSystemExtensionReplacementActionReplace;
}

- (void)requestNeedsUserApproval:(OSSystemExtensionRequest *)request
{
    fprintf(stderr, "awaiting approval in System Settings > General > Login Items & Extensions\n");
}

- (void)request:(OSSystemExtensionRequest *)request didFailWithError:(NSError *)error
{
    fprintf(stderr, "FAILED: %s (domain %s code %ld)\n",
            error.localizedDescription.UTF8String,
            error.domain.UTF8String,
            (long)error.code);
    exit(1);
}

- (void)request:(OSSystemExtensionRequest *)request
    didFinishWithResult:(OSSystemExtensionRequestResult)result
{
    /* willCompleteAfterReboot is a distinct result, not a failure -- report it as itself. */
    if (result == OSSystemExtensionRequestWillCompleteAfterReboot) {
        fprintf(stderr, "OK: completes after reboot\n");
    } else {
        fprintf(stderr, "OK: result %ld\n", (long)result);
    }
    exit(0);
}

@end

int
main(int argc, const char *argv[])
{
    @autoreleasepool {
        if (argc != 2 ||
            (strcmp(argv[1], "activate") != 0 && strcmp(argv[1], "deactivate") != 0)) {
            fprintf(stderr, "usage: %s activate|deactivate\n", argv[0]);
            return 2;
        }

        BOOL deactivate = (strcmp(argv[1], "deactivate") == 0);
        dispatch_queue_t queue = dispatch_get_main_queue();
        OSSystemExtensionRequest *request = deactivate
            ? [OSSystemExtensionRequest deactivationRequestForExtension:kDextIdentifier
                                                                queue:queue]
            : [OSSystemExtensionRequest activationRequestForExtension:kDextIdentifier
                                                                queue:queue];

        InstallerDelegate *delegate = [[InstallerDelegate alloc] init];
        request.delegate = delegate;
        [[OSSystemExtensionManager sharedManager] submitRequest:request];

        /* The delegate calls exit(); this only returns if the request never completes. */
        [[NSRunLoop mainRunLoop] run];
    }
    return 3;
}
