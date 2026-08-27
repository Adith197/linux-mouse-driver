#include <linux/module.h>
#include <linux/usb.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/input.h>
#define VENDOR_ID 0x30fa
#define PRODUCT_ID 0x0300
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Adithyaa");
MODULE_DESCRIPTION("usb mouse client driver");
MODULE_VERSION("2.0");
static int Adith_probe(struct usb_interface *interface, const struct usb_device_id *id);
static void Adith_disconnect(struct usb_interface *interface);
static void mouse_irq(struct urb *urb);
static const struct usb_device_id Adith_table[]={
    {USB_DEVICE(VENDOR_ID, PRODUCT_ID)},
    {}
};
MODULE_DEVICE_TABLE(usb, Adith_table);
static struct usb_driver mouse_driver = {
    .name = "Adith_mouse_driver",
    .id_table = Adith_table,
    .probe = Adith_probe,
    .disconnect = Adith_disconnect,
};
struct Adith_mouse{
    struct usb_device *udev;
    struct usb_interface *interface;
    struct urb *irq_urb;
    struct usb_host_endpoint *irq_endpoint;
    unsigned char *data;
    dma_addr_t data_dma;
    int data_size;
    u8 prev_buttons;
    struct input_dev *input;
};
static void handle_buttons(struct Adith_mouse *mouse);
static void handle_movement(struct Adith_mouse *mouse);
static int Adith_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
    struct usb_device *udev;
    udev = interface_to_usbdev(interface);
    struct usb_host_interface *iface_desc;
    iface_desc = interface->cur_altsetting;
    struct usb_host_endpoint *endpoint;
    struct Adith_mouse *mouse;
    mouse = kzalloc(sizeof(*mouse), GFP_KERNEL);
    if(!mouse)
    {
        printk(KERN_ERR "Out of memory\n");
        return -ENOMEM;
    }
    mouse->input = input_allocate_device();
    if(!mouse->input)
    {
        printk(KERN_ERR "failed to allocate input device\n");
        kfree(mouse);
        return -ENOMEM;
    }
    mouse->input->name = "Adith Mouse";
    mouse->input->id.bustype = BUS_USB;
    mouse->input->id.vendor = le16_to_cpu(udev->descriptor.idVendor);
    mouse->input->id.product = le16_to_cpu(udev->descriptor.idProduct);
    mouse->input->id.version = le16_to_cpu(udev->descriptor.bcdDevice);
    mouse->input->dev.parent = &interface->dev;
    /*__set_bit(EV_KEY,mouse->input->evbit);
    __set_bit(EV_REL,mouse->input->evbit);
    __set_bit(BTN_LEFT,mouse->input->keybit);
    __set_bit(BTN_RIGHT,mouse->input->keybit);
    __set_bit(BTN_MIDDLE,mouse->input->keybit);
    __set_bit(REL_X,mouse->input->relbit);
    __set_bit(REL_Y,mouse->input->relbit);
    __set_bit(REL_WHEEL,mouse->input->relbit);*/
    input_set_capability(mouse->input, EV_KEY, BTN_LEFT);
    input_set_capability(mouse->input, EV_KEY, BTN_RIGHT);
    input_set_capability(mouse->input, EV_KEY, BTN_MIDDLE);
    input_set_capability(mouse->input, EV_REL, REL_X);
    input_set_capability(mouse->input, EV_REL, REL_Y);
    input_set_capability(mouse->input, EV_REL, REL_WHEEL);
    int a;
    a = input_register_device(mouse->input);
    if(a)
    {
        printk(KERN_ERR "failed to register\n");
        input_free_device(mouse->input);
        kfree(mouse);
        return a;
    }
    mouse->udev=interface_to_usbdev(interface);
    mouse->interface=interface;
    usb_set_intfdata(interface,mouse);
    printk(KERN_INFO "USB device connected\n");
    printk(KERN_INFO "Vendor ID = 0x%04x\n", le16_to_cpu(udev->descriptor.idVendor));
    printk(KERN_INFO "Product ID = 0x%04x\n", le16_to_cpu(udev->descriptor.idProduct));
    pr_info("interface number %d\n", iface_desc->desc.bInterfaceNumber);
    pr_info("number of endpoints %d\n", iface_desc->desc.bNumEndpoints);
    pr_info("interface class %d\n", iface_desc->desc.bInterfaceClass);
    pr_info("interface subclass %d\n", iface_desc->desc.bInterfaceSubClass);
    pr_info("interface protocol %d\n", iface_desc->desc.bInterfaceProtocol);
    for(int i=0;i<iface_desc->desc.bNumEndpoints;i++)
    {
        endpoint = &iface_desc->endpoint[i];
        pr_info("endpoint address %d\n : 0x%02x\n", i, endpoint->desc.bEndpointAddress);
        if(usb_endpoint_dir_in(&endpoint->desc))
        {
            pr_info("direction: IN\n");
        }
        else
        {
            pr_info("direction: OUT\n");
        }
        pr_info("Endpoint number %d\n", usb_endpoint_num(&endpoint->desc));
        if (usb_endpoint_is_int_in(&endpoint->desc))
        {
            mouse->irq_endpoint = endpoint;
            mouse->data_size = usb_endpoint_maxp(&endpoint->desc);
        }

    }
    if(!mouse->irq_endpoint)
    {
        printk(KERN_ERR "No interrupt endpoint found\n");
        usb_free_coherent(mouse->udev, 8, mouse->data, mouse->data_dma);
        usb_free_urb(mouse->irq_urb);
        usb_set_intfdata(interface, NULL);
        kfree(mouse);
        return -ENODEV;
    }
    mouse->irq_urb=usb_alloc_urb(0, GFP_KERNEL);
    if(!mouse->irq_urb)
    {
        printk(KERN_ERR "Failed to allocate URB\n");
        usb_set_intfdata(interface, NULL);
        kfree(mouse);
        return -ENOMEM;
    }
    mouse->data=usb_alloc_coherent(mouse->udev,mouse->data_size,GFP_KERNEL,&mouse->data_dma);
    if(!mouse->data)
    {
        usb_free_urb(mouse->irq_urb);
        mouse->irq_urb = NULL;
        usb_set_intfdata(interface,NULL);
        kfree(mouse);
        return -ENOMEM;
    }
    usb_fill_int_urb(mouse->irq_urb,mouse->udev,
        usb_rcvintpipe(mouse->udev,usb_endpoint_num(&mouse->irq_endpoint->desc)),
        mouse->data,
        mouse->data_size,
        mouse_irq,
        mouse,
        mouse->irq_endpoint->desc.bInterval
    );
    int ret;
    ret = usb_submit_urb(mouse->irq_urb,GFP_KERNEL);
    if(ret)
    {
        pr_err("failed to submit urb:%d\n",ret);
        usb_free_coherent(mouse->udev,mouse->data_size,mouse->data,mouse->data_dma);
        usb_free_urb(mouse->irq_urb);
        usb_set_intfdata(interface, NULL);
        kfree(mouse);
        return ret;
    }
    pr_info("URB submitted successfully\n");
    return 0;
}
static void Adith_disconnect(struct usb_interface *interface)
{
    struct Adith_mouse *mouse;
    mouse = usb_get_intfdata(interface);
    usb_set_intfdata(interface, NULL);
    if(mouse->irq_urb)
    {
        usb_free_urb(mouse->irq_urb);
    }
    if(mouse->data)
    {
        usb_free_coherent(mouse->udev, 8, mouse->data, mouse->data_dma);
    }
    if(mouse->input)
    {
        input_unregister_device(mouse->input);
        mouse->input = NULL;
    }
    kfree(mouse);
    printk(KERN_INFO "USB device disconnected\n");
}
static void mouse_irq(struct urb *urb)
{
    struct Adith_mouse *mouse;
    mouse = urb->context;
    if(urb->status)
    {
        pr_err("URB failed %d\n",urb->status);
        return;
    }
    pr_info("URB completed\n");
    pr_info("no of bytes transferred %d\n",urb->actual_length);
    /*int i;
    for(i=0;i<urb->actual_length;i++)
    {
        pr_info("byte[%d] = 0x%02x\n",i,mouse->data[i]);
    }*/
    if(mouse->data[0] & 0x01)
    {
        pr_info("left button\n");
    }
    if(mouse->data[0] & 0x02)
    {
        pr_info("right button\n");
    }
    if(mouse->data[0] & 0x04)
    {
        pr_info("middle button\n");
    }
    handle_buttons(mouse);
    handle_movement(mouse);
    input_sync(mouse->input);
    int ret;
    ret =usb_submit_urb(urb,GFP_ATOMIC);
    if(ret)
    {
        pr_err("failed to resubmit urb:%d\n",ret);
    }
}
static void handle_buttons(struct Adith_mouse *mouse)
{
    u8 curr_buttons = mouse->data[0];
    if((curr_buttons^mouse->prev_buttons)&0x01)
    {
        if(curr_buttons & 0x01)
        {
            pr_info("left button pressed\n");
            input_report_key(mouse->input, BTN_LEFT, 1);
        }
        else
        {
            pr_info("left button released\n");
            input_report_key(mouse->input, BTN_LEFT, 0);
        }
    }
    if((curr_buttons^mouse->prev_buttons)&0x02)
    {
        if(curr_buttons & 0x02)
        {
            pr_info("right button pressed\n");
            input_report_key(mouse->input, BTN_RIGHT, 1);
        }
        else
        {
            pr_info("right button released\n");
            input_report_key(mouse->input, BTN_RIGHT, 0);
        }
    }
    if((curr_buttons^mouse->prev_buttons)&0x04)
    {
        if(curr_buttons & 0x04)
        {
            pr_info("middle button pressed\n");
            input_report_key(mouse->input, BTN_MIDDLE, 1);
        }
        else
        {
            pr_info("middle button released\n");
            input_report_key(mouse->input, BTN_MIDDLE, 0);
        }
    }
    mouse->prev_buttons = curr_buttons;
}
static void handle_movement(struct Adith_mouse *mouse)
{
    int x = (int8_t)mouse->data[1];
    int y = (int8_t)mouse->data[2];
    int wheel = (int8_t)mouse->data[4];
    pr_info("x = %d\n", x);
    pr_info("y = %d\n", y);
    pr_info("wheel = %d\n", wheel);
    input_report_rel(mouse->input, REL_X,x);
    input_report_rel(mouse->input, REL_Y,y);
    input_report_rel(mouse->input, REL_WHEEL,wheel);
}
static int __init Adith_init(void)
{
    int ret;
    ret = usb_register(&mouse_driver);
    if (ret)
    {
        printk(KERN_ERR "Registration failed\n");
        return ret;
    }
    printk(KERN_INFO "Driver registered successfully\n");
    return 0;
}
static void __exit Adith_exit(void)
{
    usb_deregister(&mouse_driver);
    printk(KERN_INFO "Driver deregistered successfully\n");
}
module_init(Adith_init);
module_exit(Adith_exit);