#include <linux/slab.h>

#include "neuron_arch.h"
#include "neuron_dhal.h"

struct neuron_dhal *ndhal = NULL;

static DEFINE_MUTEX(ndhal_init_lock);   // mutex lock to ensure single init of ndhal
int neuron_dhal_init(unsigned int pci_device_id) {
    int ret = 0;

    // ndhal is a global struct so its init must be done only once
    if (ndhal) {
        return 0;
    }

    mutex_lock(&ndhal_init_lock);
    if (!ndhal) { // double check ndhal to prevent race condition
        // allocate memory for ndhal
        ndhal = kmalloc(sizeof(struct neuron_dhal), GFP_KERNEL);
        if (ndhal == NULL) {
            pr_err("Can't allocate memory for neuron_dhal.\n");
            mutex_unlock(&ndhal_init_lock);
            return -ENOMEM;
        }
    } else {
        mutex_unlock(&ndhal_init_lock);
        return 0;
    }
    mutex_unlock(&ndhal_init_lock);

    ndhal->arch = narch_get_arch();
    ndhal->pci_device_id = pci_device_id;
    ret = ndhal_register_funcs_vc();
    switch (ndhal->arch) {
        case NEURON_ARCH_V1:
            ret = ndhal_register_funcs_v1();
            break;
        case NEURON_ARCH_V2:
            ret = ndhal_register_funcs_v2();
            break;
        case NEURON_ARCH_V3:
            ret = ndhal_register_funcs_v3();
            break;
        default:
            pr_err("Unknown HW architecture: %d. Can't init neuron_dhal.\n", ndhal->arch);
            return -EINVAL;
    }

    return ret;
}

void neuron_dhal_cleanup(void)
{
    if (ndhal) {
    	if (ndhal->ndhal_ext_cleanup) {
    		ndhal->ndhal_ext_cleanup();
		}
	}
}

void neuron_dhal_free(void)
{
    if (ndhal) {
        kfree(ndhal);
	}
}
