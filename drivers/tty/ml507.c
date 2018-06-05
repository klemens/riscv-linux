// SPDX-License-Identifier: GPL-2.0
/*
 * Simple terminal tty console driver
 *
 * Based mostly on the goldfish driver by Google, with some parts
 * from the SiFive uart driver.
 */

#include <linux/console.h>
#include <linux/platform_device.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/serial_core.h>
#include <linux/of.h>

struct ml507_tty {
	struct tty_port port;
	void __iomem *base;
	struct console console;
	struct device *dev;
};

static DEFINE_MUTEX(ml507_tty_lock);
static struct tty_driver *ml507_tty_driver;
static u32 ml507_tty_line_count = 8;
static u32 ml507_tty_current_line_count;
static struct ml507_tty *ml507_ttys;

static void ml507_tty_do_write(int line, const char *buf,
				  unsigned int count)
{
	struct ml507_tty *qtty = &ml507_ttys[line];
	void __iomem *base = qtty->base;
	size_t i;

	for(i = 0; i < count; ++i) {
		writel(buf[i], base);
	}
}

static int ml507_tty_activate(struct tty_port *port, struct tty_struct *tty)
{
	return 0;
}

static void ml507_tty_shutdown(struct tty_port *port)
{
}

static int ml507_tty_open(struct tty_struct *tty, struct file *filp)
{
	struct ml507_tty *qtty = &ml507_ttys[tty->index];
	return tty_port_open(&qtty->port, tty, filp);
}

static void ml507_tty_close(struct tty_struct *tty, struct file *filp)
{
	tty_port_close(tty->port, tty, filp);
}

static void ml507_tty_hangup(struct tty_struct *tty)
{
	tty_port_hangup(tty->port);
}

static int ml507_tty_write(struct tty_struct *tty, const unsigned char *buf,
								int count)
{
	ml507_tty_do_write(tty->index, buf, count);
	return count;
}

static int ml507_tty_write_room(struct tty_struct *tty)
{
	return 0x10000;
}

static void ml507_tty_console_write(struct console *co, const char *b,
								unsigned count)
{
	ml507_tty_do_write(co->index, b, count);
}

static struct tty_driver *ml507_tty_console_device(struct console *c,
								int *index)
{
	*index = c->index;
	return ml507_tty_driver;
}

static int ml507_tty_console_setup(struct console *co, char *options)
{
	if ((unsigned)co->index >= ml507_tty_line_count)
		return -ENODEV;
	if (!ml507_ttys[co->index].base)
		return -ENODEV;
	return 0;
}

static const struct tty_port_operations ml507_port_ops = {
	.activate = ml507_tty_activate,
	.shutdown = ml507_tty_shutdown
};

static const struct tty_operations ml507_tty_ops = {
	.open = ml507_tty_open,
	.close = ml507_tty_close,
	.hangup = ml507_tty_hangup,
	.write = ml507_tty_write,
	.write_room = ml507_tty_write_room,
};

static int ml507_tty_create_driver(void)
{
	int ret;
	struct tty_driver *tty;

	ml507_ttys = kzalloc(sizeof(*ml507_ttys) *
				ml507_tty_line_count, GFP_KERNEL);
	if (ml507_ttys == NULL) {
		ret = -ENOMEM;
		goto err_alloc_ml507_ttys_failed;
	}
	tty = alloc_tty_driver(ml507_tty_line_count);
	if (tty == NULL) {
		ret = -ENOMEM;
		goto err_alloc_tty_driver_failed;
	}
	tty->driver_name = "ml507";
	tty->name = "ttyML";
	tty->type = TTY_DRIVER_TYPE_SERIAL;
	tty->subtype = SERIAL_TYPE_NORMAL;
	tty->init_termios = tty_std_termios;
	tty->flags = TTY_DRIVER_RESET_TERMIOS | TTY_DRIVER_REAL_RAW |
						TTY_DRIVER_DYNAMIC_DEV;
	tty_set_operations(tty, &ml507_tty_ops);
	ret = tty_register_driver(tty);
	if (ret)
		goto err_tty_register_driver_failed;

	ml507_tty_driver = tty;
	return 0;

err_tty_register_driver_failed:
	put_tty_driver(tty);
err_alloc_tty_driver_failed:
	kfree(ml507_ttys);
	ml507_ttys = NULL;
err_alloc_ml507_ttys_failed:
	return ret;
}

static void ml507_tty_delete_driver(void)
{
	tty_unregister_driver(ml507_tty_driver);
	put_tty_driver(ml507_tty_driver);
	ml507_tty_driver = NULL;
	kfree(ml507_ttys);
	ml507_ttys = NULL;
}

static int ml507_tty_probe(struct platform_device *pdev)
{
	struct ml507_tty *qtty;
	int ret = -ENODEV;
	struct resource *r;
	struct device *ttydev;
	void __iomem *base;
	unsigned int line;

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!r) {
		pr_err("ml507_tty: No MEM resource available!\n");
		return -ENOMEM;
	}

	base = devm_ioremap_resource(&pdev->dev, r);
	if (IS_ERR(base)) {
		pr_err("ml507_tty: Unable to ioremap base!\n");
		return -ENOMEM;
	}
	writel('H', base);
	writel('i', base);

	mutex_lock(&ml507_tty_lock);

	if (pdev->id == PLATFORM_DEVID_NONE)
		line = ml507_tty_current_line_count;
	else
		line = pdev->id;

	if (line >= ml507_tty_line_count) {
		pr_err("ml507_tty: Reached maximum tty number of %d.\n",
		       ml507_tty_current_line_count);
		ret = -ENOMEM;
		goto err_unlock;
	}

	if (ml507_tty_current_line_count == 0) {
		ret = ml507_tty_create_driver();
		if (ret)
			goto err_unlock;
	}
	ml507_tty_current_line_count++;

	qtty = &ml507_ttys[line];
	tty_port_init(&qtty->port);
	qtty->port.ops = &ml507_port_ops;
	qtty->base = base;
	qtty->dev = &pdev->dev;

	ttydev = tty_port_register_device(&qtty->port, ml507_tty_driver,
					  line, &pdev->dev);
	if (IS_ERR(ttydev)) {
		ret = PTR_ERR(ttydev);
		goto err_tty_register_device_failed;
	}

	strcpy(qtty->console.name, "ttyML");
	qtty->console.write = ml507_tty_console_write;
	qtty->console.device = ml507_tty_console_device;
	qtty->console.setup = ml507_tty_console_setup;
	qtty->console.flags = CON_PRINTBUFFER;
	qtty->console.index = line;
	register_console(&qtty->console);
	platform_set_drvdata(pdev, qtty);

	mutex_unlock(&ml507_tty_lock);
	return 0;

err_tty_register_device_failed:
	ml507_tty_current_line_count--;
	if (ml507_tty_current_line_count == 0)
		ml507_tty_delete_driver();
err_unlock:
	mutex_unlock(&ml507_tty_lock);
	iounmap(base);
	return ret;
}

static int ml507_tty_remove(struct platform_device *pdev)
{
	struct ml507_tty *qtty = platform_get_drvdata(pdev);

	mutex_lock(&ml507_tty_lock);

	unregister_console(&qtty->console);
	tty_unregister_device(ml507_tty_driver, qtty->console.index);
	iounmap(qtty->base);
	qtty->base = NULL;
	ml507_tty_current_line_count--;
	if (ml507_tty_current_line_count == 0)
		ml507_tty_delete_driver();
	mutex_unlock(&ml507_tty_lock);
	return 0;
}

static void ml507_early_console_putchar(struct uart_port *port, int ch)
{
	writel(ch, port->membase);
}

static void ml507_early_write(struct console *con, const char *s, unsigned int n)
{
	struct earlycon_device *dev = con->data;

	uart_console_write(&dev->port, s, n, ml507_early_console_putchar);
}

static int __init ml507_earlycon_setup(struct earlycon_device *device,
				    const char *opt)
{
	struct uart_port *port = &device->port;

	if (!(port->membase || port->iobase))
		return -ENODEV;

	device->con->write = ml507_early_write;
	return 0;
}

OF_EARLYCON_DECLARE(early_ml_tty, "klemens,terminal0", ml507_earlycon_setup);

static const struct of_device_id ml507_tty_of_match[] = {
	{ .compatible = "klemens,terminal0", },
	{},
};

MODULE_DEVICE_TABLE(of, ml507_tty_of_match);

static struct platform_driver ml507_tty_platform_driver = {
	.probe = ml507_tty_probe,
	.remove = ml507_tty_remove,
	.driver = {
		.name = "ml507_tty",
		.of_match_table = of_match_ptr(ml507_tty_of_match),
	}
};

module_platform_driver(ml507_tty_platform_driver);

MODULE_DESCRIPTION("ML507 TTY console driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Klemens Schölhorn <klemens@schoelhorn.eu>");
