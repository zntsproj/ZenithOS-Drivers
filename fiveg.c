/**
 * @file fiveg.c
 *
 * This file contains the implementation of a simplified 5G driver for
 * interacting with 5G modems and managing network connections.
 *
 * Driver version: 2.8
 * This software is licensed under the MIT License.

  * This driver is under development and has been tested on 3 devices.
 * It may not even transmit signals yet! But there are many more tests to come...
 *
 * Developed by NE5LINK (znts543@gmail.com)

 * upd: 5G-Stream 2.8 released, we can add skbuff.h!
 *
 * We welcome forks and contributions to this project!  Feel free to contribute.

 */

/*RUN ONLY WITH ROOT! and not with GCC Compiler.  */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/socket.h>
#include <linux/udp.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/inet.h>
#include <asm/uaccess.h>
#include <linux/skbuff.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/ip.h>
#include <linux/icmp.h>
#include <linux/jiffies.h>
#include <linux/rfkill.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#ifndef RFKILL_TYPE_CELLULAR
#define RFKILL_TYPE_CELLULAR 7
#endif

#define DRIVER_NAME "fiveg_driver"

#define ICCID_REGISTER_OFFSET 0x100
#define ICCID_LENGTH 20
#define ANTENNA_POWER_REGISTER_OFFSET 0x200 

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ne5link, MAIN DEV of ZenithOS.");
MODULE_DESCRIPTION("5G driver with antenna control. IN TEST");

struct fiveg_connection {
    struct socket *sock;
    struct sockaddr_in server_addr;
    struct rfkill *rfkill;
    char *mec_server_address;
    int mec_server_port;
};

static bool fiveg_rfkill_set_block(void *data, bool blocked);

static const struct rfkill_ops fiveg_rfkill_ops = {
    .set_block = fiveg_rfkill_set_block,
};

static void __iomem *base_register;
static struct fiveg_connection *conn;

static int fiveg_send_data(struct fiveg_connection *conn, const void *data, size_t len) {
    struct msghdr msg;
    struct kvec iov;
    int ret;

    memset(&msg, 0, sizeof(msg));
    msg.msg_name = &conn->server_addr;
    msg.msg_namelen = sizeof(conn->server_addr);

    iov.iov_base = (void *)data;
    iov.iov_len = len;

    ret = kernel_sendmsg(conn->sock, &msg, &iov, 1, len);
    if (ret < 0) {
        printk(KERN_ERR "Failed to send data: %d\n", ret);
        return ret;
    } else if (ret != len) {
        printk(KERN_WARNING "Sent only %d bytes out of %zu\n", ret, len);
    }

    return ret;
}

static char *fiveg_get_iccid(void) {
    char *iccid;
    int i;

    iccid = kmalloc(ICCID_LENGTH + 1, GFP_KERNEL);
    if (!iccid) {
        return NULL;
    }

    for (i = 0; i < ICCID_LENGTH; i++) {
        iccid[i] = readb(base_register + ICCID_REGISTER_OFFSET + i);
    }

    iccid[ICCID_LENGTH] = '\0';

    return iccid;
}

static int fiveg_connect(struct fiveg_connection *conn, const char *ip, int port, struct device *dev) {
    int ret;

    conn->rfkill = rfkill_alloc("5g-modem", dev, RFKILL_TYPE_CELLULAR, &fiveg_rfkill_ops, conn);
    if (!conn->rfkill) {
        printk(KERN_ERR "Failed to allocate rfkill switch\n");
        return -ENOMEM;
    }

    ret = rfkill_register(conn->rfkill);
    if (ret < 0) {
        rfkill_destroy(conn->rfkill);
        printk(KERN_ERR "Failed to register rfkill switch\n");
        return ret;
    }

    ret = sock_create_kern(&init_net, PF_INET, SOCK_DGRAM, IPPROTO_UDP, &conn->sock);
    if (ret < 0) {
        printk(KERN_ERR "Failed to create socket: %d\n", ret);
        rfkill_unregister(conn->rfkill);
        rfkill_destroy(conn->rfkill);
        return ret;
    }

    memset(&conn->server_addr, 0, sizeof(conn->server_addr));
    conn->server_addr.sin_family = AF_INET;
    conn->server_addr.sin_port = htons(port);
    ret = in4_pton(ip, -1, (u8 *)&conn->server_addr.sin_addr.s_addr, '\0', NULL);

    if (ret != 1) {
        printk(KERN_ERR "Invalid IP address: %s\n", ip);
        sock_release(conn->sock);
        rfkill_unregister(conn->rfkill);
        rfkill_destroy(conn->rfkill);
        return -EINVAL;
    }

    return 0;
}

static bool fiveg_rfkill_set_block(void *data, bool blocked) {
    struct fiveg_connection *conn = data;

    if (blocked) {

    } else {

    }
    return true;
}

static ssize_t antenna_power_show(struct device *dev, struct device_attribute *attr, char *buf) {
    u8 power_state = readb(base_register + ANTENNA_POWER_REGISTER_OFFSET);
    return sprintf(buf, "%u\n", power_state);
}

static ssize_t antenna_power_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count) {
    unsigned long power_state;
    int ret;

    ret = kstrtoul(buf, 0, &power_state);
    if (ret)
        return ret;

    if (power_state == 0) {
        writeb(0, base_register + ANTENNA_POWER_REGISTER_OFFSET);
    } else if (power_state == 1){
        writeb(1, base_register + ANTENNA_POWER_REGISTER_OFFSET);
    }

    return count;
}

static DEVICE_ATTR(antenna_power, 0644, antenna_power_show, antenna_power_store);

static int fiveg_probe(struct platform_device *pdev) {
    int ret;
    struct device *dev = &pdev->dev;
    struct resource *res;
    printk(KERN_INFO "%s: Probing...\n", DRIVER_NAME);

    char *iccid = fiveg_get_iccid();
    if (iccid) {
        printk(KERN_INFO "ICCID: %s\n", iccid);
        kfree(iccid);
    } else {
        printk(KERN_ERR "Failed to read ICCID\n");
    }

    conn = kzalloc(sizeof(*conn), GFP_KERNEL);
    if (!conn) {
        return -ENOMEM;
    }

    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (!res) {
        ret = -ENODEV;
        goto err_free_conn;
    }

    base_register = devm_ioremap_resource(dev, res);
    if (IS_ERR(base_register)) {
        ret = PTR_ERR(base_register);
        goto err_free_conn;
    }

    ret = fiveg_connect(conn, "192.168.1.100", 8944, dev);
    if (ret < 0) {
        goto err_free_conn;
    }

    char *message = "5G Driver - For ARM or X86 [in test]\n";
    ret = fiveg_send_data(conn, message, strlen(message));
    if (ret < 0) {
        goto err_rfkill;
    }

 ret = device_create_file(dev, &dev_attr_antenna_power);
    if (ret) {
        dev_err(dev, "Failed to create sysfs attributes\n");
        goto err_rfkill;
    }

    platform_set_drvdata(pdev, conn);
    printk(KERN_INFO "%s: Probed successfully\n", DRIVER_NAME);
    return 0;

err_rfkill:
    rfkill_unregister(conn->rfkill);
    rfkill_destroy(conn->rfkill);
    sock_release(conn->sock);
    kfree(conn); // освобождаем conn в err_rfkill
    return ret;

err_free_conn:
    kfree(conn);
    return ret;

}

static void fiveg_remove(struct platform_device *pdev) {

    struct fiveg_connection *conn = platform_get_drvdata(pdev);
    device_remove_file(&pdev->dev, &dev_attr_antenna_power);

    if (conn && conn->rfkill) {
        rfkill_unregister(conn->rfkill);
        rfkill_destroy(conn->rfkill);
    }

    if (conn && conn->sock) {
        sock_release(conn->sock);
        kfree(conn);
    }

    printk(KERN_INFO "%s: Removed\n", DRIVER_NAME);
}

static struct platform_driver fiveg_driver = {
    .probe = fiveg_probe,
    .remove = fiveg_remove,
    .driver = {
        .name = DRIVER_NAME,
    },
};

module_platform_driver(fiveg_driver);
