/*
	Pulse reader reads pulses duty and cycle 
 */

#include <linux/module.h>
#include <linux/cdev.h>            
#include <linux/ktime.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/gpio.h>
#include <linux/hrtimer.h>
#include <linux/sort.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/slab.h>

// #define PULSE_READER_DEBUG

//maxium io supported
#define	MAX_IO_NUMBER				10

//max pulse width that can be detected
//it must longer than the period of any pwm monitored
//the pulse over limit would cause buffer clear to 0
#define	MAX_PULSE_WIDTH				30//in ms

//this is the max allowed median filter window size
#define	MAX_FILTER_WINDOW_SIZE		48
#define	MIN_FILTER_WINDOW_SIZE		1//no filter
#define	DEFALT_FILTER_WINDOW_SIZE	3

#define	MAX_CALCULATE_PERIOD		1000//in ms
#define	MIN_CALCULATE_PERIOD		10
#define	DEFALT_CALCULATE_PERIOD		10

#define	ADD_IO						0x7B01
#define	REMOVE_IO					0x7B02
#define	SET_CAL_PERIOD				0x7B03//set calculate_period in ms
#define	GET_IO_STAT					0x7B04

typedef struct
{
	uint32_t gpio;
	uint32_t filter_win_size;
} add_io_t;

typedef struct
{
	uint32_t gpio;
	uint32_t duty;//in nanosecond
	uint32_t cycle;//in nanosecond
} io_stat_user_t;

typedef struct
{
	io_stat_user_t io_stat_user[MAX_IO_NUMBER];
	uint32_t n_ios;
} get_io_stat_t;

typedef struct
{
	uint32_t gpio;
	uint32_t filter_win_size;
	uint32_t irq;
	bool used;

	//runtime stats
	uint8_t level;
	ktime_t pulse_p[MAX_FILTER_WINDOW_SIZE];
	ktime_t pulse_n[MAX_FILTER_WINDOW_SIZE];
	uint32_t index_p;
	uint32_t index_n;
	ktime_t last_edge;//time since last edge
	ktime_t last_cycle;//pulse width in last timer cycle
	bool stopped;
} io_stat_t;

struct pulse_reader_dev_t {
	struct cdev cdev;

	spinlock_t rlock;

	io_stat_t io_stats[MAX_IO_NUMBER];
	uint32_t calculate_period;//in ms

	struct hrtimer pulse_timer;
};

static struct class *pulse_reader_class;
static struct pulse_reader_dev_t *pulse_reader_dev;

static int pulse_reader_major;
static int pulse_reader_minor;

int pulse_reader_open(struct inode *inode, struct file *filp)
{
	struct pulse_reader_dev_t *p_dev;
	int num = MINOR(inode->i_rdev);

	if (num != pulse_reader_minor) {
		printk(KERN_ERR "pulse_reader_open open error\n");
		return -ENODEV;
	}

	if ( !filp->private_data ) {
		p_dev = pulse_reader_dev;
		filp->private_data = p_dev;
	} else {
		p_dev = (struct pulse_reader_dev_t*) filp->private_data;
	}

	return 0;
}

int pulse_reader_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static void pulse_reader_stat_reset(io_stat_t *p_stat)
{
	int i;
	p_stat->level = gpio_get_value(p_stat->gpio);
	for(i=0; i<MAX_FILTER_WINDOW_SIZE; i++) {
		p_stat->pulse_p[i] = ktime_set(0, 0);
		p_stat->pulse_n[i] = ktime_set(0, 0);
	}
	p_stat->index_p = 0;
	p_stat->index_n = 0;
	p_stat->last_edge = ktime_set(0, 0);
	p_stat->last_cycle = ktime_set(0, 0);
	p_stat->stopped = true;
}

static int pulse_reader_sort_cmp_func(const void *opp1, const void *opp2)
{
	ktime_t *t1 = (ktime_t*)opp1, *t2 = (ktime_t*)opp2;
	return ktime_compare(*t1, *t2);
}

static void pulse_reader_filter_and_calc(io_stat_t *p_stat, uint32_t *duty, uint32_t *cycle)
{
#ifdef PULSE_READER_DEBUG
	printk(KERN_DEBUG  "pulse_reader_filter_and_calc before p-n=%u-%u %u-%u %u-%u %u-%u %u-%u\n",
		(uint32_t)ktime_to_us(p_stat->pulse_p[0]),
		(uint32_t)ktime_to_us(p_stat->pulse_n[0]),
		(uint32_t)ktime_to_us(p_stat->pulse_p[1]),
		(uint32_t)ktime_to_us(p_stat->pulse_n[1]),
		(uint32_t)ktime_to_us(p_stat->pulse_p[2]),
		(uint32_t)ktime_to_us(p_stat->pulse_n[2]),
		(uint32_t)ktime_to_us(p_stat->pulse_p[3]),
		(uint32_t)ktime_to_us(p_stat->pulse_n[3]),
		(uint32_t)ktime_to_us(p_stat->pulse_p[4]),
		(uint32_t)ktime_to_us(p_stat->pulse_n[4]));
#endif

	if(p_stat->filter_win_size > 1) {
		//run median filter
		sort((void*)p_stat->pulse_p, p_stat->filter_win_size, sizeof(ktime_t), pulse_reader_sort_cmp_func, NULL);
		sort((void*)p_stat->pulse_n, p_stat->filter_win_size, sizeof(ktime_t), pulse_reader_sort_cmp_func, NULL);

		//get the median value and calculate cycle
		*duty = ktime_to_ns(p_stat->pulse_p[p_stat->filter_win_size/2]);
		*cycle = ktime_to_ns(ktime_add(p_stat->pulse_p[p_stat->filter_win_size/2],
			p_stat->pulse_n[p_stat->filter_win_size/2]));
	} else {
		*duty = ktime_to_ns(p_stat->pulse_p[0]);
		*cycle = ktime_to_ns(ktime_add(p_stat->pulse_p[0], p_stat->pulse_n[0]));
	}

#ifdef PULSE_READER_DEBUG
	printk(KERN_DEBUG  "pulse_reader_filter_and_calc after p-n=%u-%u %u-%u %u-%u %u-%u %u-%u\n",
		(uint32_t)ktime_to_us(p_stat->pulse_p[0]),
		(uint32_t)ktime_to_us(p_stat->pulse_n[0]),
		(uint32_t)ktime_to_us(p_stat->pulse_p[1]),
		(uint32_t)ktime_to_us(p_stat->pulse_n[1]),
		(uint32_t)ktime_to_us(p_stat->pulse_p[2]),
		(uint32_t)ktime_to_us(p_stat->pulse_n[2]),
		(uint32_t)ktime_to_us(p_stat->pulse_p[3]),
		(uint32_t)ktime_to_us(p_stat->pulse_n[3]),
		(uint32_t)ktime_to_us(p_stat->pulse_p[4]),
		(uint32_t)ktime_to_us(p_stat->pulse_n[4]));
#endif
}

static enum hrtimer_restart pulse_reader_timer_cb(struct hrtimer *timer)
{
	uint32_t i;
	unsigned long irq_flags;
	struct pulse_reader_dev_t *p_dev =
		container_of(timer, struct pulse_reader_dev_t, pulse_timer);
	ktime_t t_current;

	//get current time
	t_current = hrtimer_cb_get_time(&p_dev->pulse_timer);

	spin_lock_irqsave(&p_dev->rlock, irq_flags);
	for(i=0; i<MAX_IO_NUMBER; i++) {
		io_stat_t *p_stat;

		p_stat = &(p_dev->io_stats[i]);

		if(!(p_stat->used) || p_stat->stopped)
			continue;

#ifdef PULSE_READER_DEBUG
		printk(KERN_DEBUG  "pulse_reader_timer_cb lastc=%u, laste=%u\n",
			(uint32_t)ktime_to_us(p_stat->last_cycle),
			(uint32_t)ktime_to_us(p_stat->last_edge));
#endif

		if(ktime_to_ns(p_stat->last_cycle) != 0) {
			//none zero means there's no edge in last period
			p_stat->last_cycle = ktime_add(ms_to_ktime(p_dev->calculate_period), p_stat->last_cycle);
			if(ktime_compare(p_stat->last_cycle, ms_to_ktime(MAX_PULSE_WIDTH)) > 0) {
				//pulse stopped, reset data and set stop flag
				pulse_reader_stat_reset(p_stat);
#ifdef PULSE_READER_DEBUG
				printk(KERN_DEBUG  "pulse_reader_timer_cb pulse on gpio %d stopped\n", p_stat->gpio);
#endif
				continue;
			}
		} else {
			//record the part of time in last period
			p_stat->last_cycle = ktime_sub(t_current, p_stat->last_edge);
		}
		//set the last edge position to current
		p_stat->last_edge = t_current;

#ifdef PULSE_READER_DEBUG
		printk(KERN_DEBUG  "pulse_reader_timer_cb cur=%u, lastc=%u, laste=%u\n",
			(uint32_t)ktime_to_us(t_current),
			(uint32_t)ktime_to_us(p_stat->last_cycle),
			(uint32_t)ktime_to_us(p_stat->last_edge));
#endif
	}
	spin_unlock_irqrestore(&p_dev->rlock, irq_flags);

	//restart timer
	hrtimer_forward_now(&p_dev->pulse_timer, ms_to_ktime(p_dev->calculate_period));
	return HRTIMER_RESTART;
}

static irqreturn_t pulse_reader_io_interrupt(int irq, void *dev_id)
{
	uint32_t i;
	io_stat_t *p_stat;
	unsigned long irq_flags;
	ktime_t t_current, t_width;
	struct pulse_reader_dev_t *p_dev = (struct pulse_reader_dev_t *) dev_id;
	uint8_t new_level;

	//get current timestamp
	t_current = hrtimer_cb_get_time(&p_dev->pulse_timer);

	spin_lock_irqsave(&p_dev->rlock, irq_flags);

	for(i=0; i<MAX_IO_NUMBER; i++) {
		if(p_dev->io_stats[i].irq == irq
			&& p_dev->io_stats[i].used)
			break;
	}
	if(i == MAX_IO_NUMBER) {
		spin_unlock_irqrestore(&p_dev->rlock, irq_flags);
		printk(KERN_ERR "pulse_reader_io_interrupt unknown irq %d\n", irq);
		return IRQ_NONE;
	}
	p_stat = &(p_dev->io_stats[i]);

	//check and ignore the interrupt
	//in case it generates fake ones while there's no actual edge
	new_level = gpio_get_value(p_stat->gpio);
	if(p_stat->level == new_level) {
		spin_unlock_irqrestore(&p_dev->rlock, irq_flags);
		return IRQ_HANDLED;
	}
	p_stat->level = new_level;

	//calculate width and update stop flag
	if(p_stat->stopped) {
		//avoid very long pulse
		t_width = ktime_set(0, 0);
		p_stat->stopped = false;
	} else {
		t_width = ktime_sub(t_current, p_stat->last_edge);
	}

#ifdef PULSE_READER_DEBUG
	printk(KERN_DEBUG "pulse_reader_io_interrupt gpio=%u, tcur=%u, width=%u, laste=%u, lastc=%u, pi-ni=%u-%u, pol=%c\n",
		p_stat->gpio,
		(uint32_t)ktime_to_us(t_current),
		(uint32_t)ktime_to_us(t_width),
		(uint32_t)ktime_to_us(p_stat->last_edge),
		(uint32_t)ktime_to_us(p_stat->last_cycle),
		p_stat->index_p, p_stat->index_n,
		p_stat->level ? 'N' : 'P');
#endif

	p_stat->last_edge = t_current;

	//if there's part of the pulse in last timer cycle, add it
	if(ktime_to_ns(p_stat->last_cycle) != 0) {
		t_width = ktime_add(t_width, p_stat->last_cycle);
		p_stat->last_cycle = ktime_set(0, 0);
	}

	//store width in either positive pulse array or negative array
	//jump to next once both items have value
	if(p_stat->level == 0) {
		//falling edge, calculate the positive pulse width
		p_stat->pulse_p[p_stat->index_p] = t_width;
		p_stat->index_p++;
		if(p_stat->index_p >= p_stat->filter_win_size)
			p_stat->index_p = 0;
	} else if(p_stat->level == 1) {
		//raising edge, calculate the negative pulse width
		p_stat->pulse_n[p_stat->index_n] = t_width;
		p_stat->index_n++;
		if(p_stat->index_n >= p_stat->filter_win_size)
			p_stat->index_n = 0;
	} else {
		pulse_reader_stat_reset(p_stat);
		printk(KERN_ERR "pulse_reader_io_interrupt gpio_get_value returns %d!\n", p_stat->level);
	}

	spin_unlock_irqrestore(&p_dev->rlock, irq_flags);

	return IRQ_HANDLED;
}

//static int pulse_reader_ioctl(struct inode * inode,struct file* filp, unsigned int cmd, unsigned long arg)
static long pulse_reader_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	unsigned long irq_flags;
	struct pulse_reader_dev_t *p_dev = (struct pulse_reader_dev_t *) file->private_data;

	switch (cmd)
	{
	case ADD_IO:
		{
			int io_irq, ret, i;
			add_io_t add_io;

			if(copy_from_user(&add_io, (void *)arg, sizeof(add_io_t)))
				return -EFAULT;

			if(!gpio_is_valid(add_io.gpio))
				return -EFAULT;

			spin_lock_irqsave(&p_dev->rlock, irq_flags);

			//repeat adding check
			for(i=0; i<MAX_IO_NUMBER; i++) {
				if(p_dev->io_stats[i].gpio == add_io.gpio
					&& p_dev->io_stats[i].used) {
					//already added, just reuse
					spin_unlock_irqrestore(&p_dev->rlock, irq_flags);
					printk(KERN_INFO "pulse_reader_ioctl request ADD_IO io already added\n");
					return 0;
				}
			}

			//check available item if IO was not added
			for(i=0; i<MAX_IO_NUMBER; i++) {
				if(!(p_dev->io_stats[i].used))
					break;
			}
			if(i == MAX_IO_NUMBER) {
				spin_unlock_irqrestore(&p_dev->rlock, irq_flags);
				printk(KERN_ERR "pulse_reader_ioctl ADD_IO exceed max io number\n");
				return -EFAULT;
			}

			//request gpio
			ret = gpio_request(add_io.gpio, "pulse_reader");
			if(ret) {
				spin_unlock_irqrestore(&p_dev->rlock, irq_flags);
				printk(KERN_ERR "pulse_reader_ioctl ADD_IO request io error\n");
				return ret;
			}
			gpio_direction_input(add_io.gpio);
			p_dev->io_stats[i].gpio = add_io.gpio;

			//request irq
			io_irq = gpio_to_irq(add_io.gpio);
			ret = request_irq(io_irq, pulse_reader_io_interrupt,
				IRQF_TRIGGER_RISING|IRQF_TRIGGER_FALLING, "pulse_reader_io_interrupt", p_dev);
			if(ret) {
				spin_unlock_irqrestore(&p_dev->rlock, irq_flags);
				printk(KERN_ERR "pulse_reader_ioctl ADD_IO can not get irq\n");
				return ret;
			}
			p_dev->io_stats[i].irq = io_irq;

			//set filter window size
			if(add_io.filter_win_size > MAX_FILTER_WINDOW_SIZE)
				add_io.filter_win_size = MAX_FILTER_WINDOW_SIZE;
			if(add_io.filter_win_size < MIN_FILTER_WINDOW_SIZE)
				add_io.filter_win_size = MIN_FILTER_WINDOW_SIZE;
			p_dev->io_stats[i].filter_win_size = add_io.filter_win_size;

			//reset data, this must be called post to gpio_request
			pulse_reader_stat_reset(&(p_dev->io_stats[i]));

			//set used flag
			p_dev->io_stats[i].used = true;

			spin_unlock_irqrestore(&p_dev->rlock, irq_flags);
		}
		break;
	case REMOVE_IO:
		{
			uint32_t gpio, i;

			if(copy_from_user(&gpio, (void *)arg, sizeof(uint32_t)))
				return -EFAULT;

			spin_lock_irqsave(&p_dev->rlock, irq_flags);

			for(i=0; i<MAX_IO_NUMBER; i++) {
				if(p_dev->io_stats[i].gpio == gpio
					&& p_dev->io_stats[i].used) {
					free_irq(p_dev->io_stats[i].irq, p_dev);
					gpio_free(p_dev->io_stats[i].gpio);
					p_dev->io_stats[i].used = false;
					break;
				}
			}
			if(i == MAX_IO_NUMBER) {
				spin_unlock_irqrestore(&p_dev->rlock, irq_flags);
				printk(KERN_ERR "pulse_reader_ioctl io not added %d\n", gpio);
				return -EFAULT;
			}

			spin_unlock_irqrestore(&p_dev->rlock, irq_flags);
		}
		break;
	case SET_CAL_PERIOD:
		{
			uint32_t period, i;

			if(copy_from_user(&period, (void *)arg, sizeof(uint32_t)))
				return -EFAULT;

			if(period > MAX_CALCULATE_PERIOD)
				period = MAX_CALCULATE_PERIOD;
			if(period < MIN_CALCULATE_PERIOD)
				period = MIN_CALCULATE_PERIOD;

			spin_lock_irqsave(&p_dev->rlock, irq_flags);
			p_dev->calculate_period = period;
			for(i=0; i<MAX_IO_NUMBER; i++)
				pulse_reader_stat_reset(&p_dev->io_stats[i]);
			hrtimer_cancel(&p_dev->pulse_timer);
			hrtimer_start(&p_dev->pulse_timer, ms_to_ktime(p_dev->calculate_period), HRTIMER_MODE_REL);
			spin_unlock_irqrestore(&p_dev->rlock, irq_flags);
		}
		break;
	case GET_IO_STAT:
		{
			uint32_t i, j;
			io_stat_t *p_stat;
			get_io_stat_t get_io_stat;

			if(copy_from_user(&get_io_stat, (void *)arg, sizeof(get_io_stat_t)))
				return -EFAULT;

			spin_lock_irqsave(&p_dev->rlock, irq_flags);
			for(j=0; j<get_io_stat.n_ios; j++) {
				for(i=0; i<MAX_IO_NUMBER; i++) {
					p_stat = &(p_dev->io_stats[i]);
					if(p_stat->gpio == get_io_stat.io_stat_user[j].gpio && p_stat->used) {
						if(!p_stat->stopped) {
							pulse_reader_filter_and_calc(
								p_stat, &(get_io_stat.io_stat_user[j].duty), &(get_io_stat.io_stat_user[j].cycle));
#ifdef PULSE_READER_DEBUG
							printk(KERN_DEBUG "pulse_reader_ioctl gpio=%u, duty=%u, cycle=%u\n",
								p_stat->gpio, get_io_stat.io_stat_user[j].duty, get_io_stat.io_stat_user[j].cycle);
#endif
						} else {
							get_io_stat.io_stat_user[j].duty = 0;
							get_io_stat.io_stat_user[j].cycle = 0;
						}
						break;
					}
				}
			}
			spin_unlock_irqrestore(&p_dev->rlock, irq_flags);

			if(copy_to_user((void *)arg, &get_io_stat, sizeof(get_io_stat_t)))
				return -EFAULT;
		}
		break;

	default:
		return 0;
		break;
	}
	return 0;
}

static const struct file_operations pulse_reader_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = pulse_reader_ioctl,
	.open = pulse_reader_open,
	.release = pulse_reader_release,
};

int pulse_reader_init(void)
{
	int err, result = -1;
	dev_t devno;

	// Allocate a dynamic major number
    result = alloc_chrdev_region(&devno, 0, 1, "pulse_reader");
    if (result < 0) {
        pr_err("pulse_reader_init failed to allocate char device region: %d\n", result);
        return result;
    }
	// Store for later cleanup or device creation
    pulse_reader_major = MAJOR(devno);
    pulse_reader_minor = MINOR(devno);

	printk(KERN_DEBUG  "pulse_reader_init dev num major=%d, minor=%d\n",
		pulse_reader_major, pulse_reader_minor);

	pulse_reader_class = class_create("pulse_reader");
	if (IS_ERR(pulse_reader_class)) {
		result = PTR_ERR(pulse_reader_class);
		printk(KERN_ERR "pulse_reader_init class_create failed: %d\n", result);
		goto fail_create_class;
	}

	// need to check the result of device_create
	device_create(pulse_reader_class, NULL,
		MKDEV(pulse_reader_major, pulse_reader_minor), NULL, "pulse_reader");

	pulse_reader_dev = kmalloc(sizeof(struct pulse_reader_dev_t), GFP_KERNEL);
	if (!pulse_reader_dev)
	{
		result = -ENOMEM;
		printk(KERN_ERR "pulse_reader_init kmalloc failed\n");
		goto fail_malloc;
	}
	memset(pulse_reader_dev, 0, sizeof(struct pulse_reader_dev_t));

	spin_lock_init(&pulse_reader_dev->rlock);
	pulse_reader_dev->calculate_period = DEFALT_CALCULATE_PERIOD;//default period is 10ms
	hrtimer_init(&pulse_reader_dev->pulse_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	pulse_reader_dev->pulse_timer.function = pulse_reader_timer_cb;
	hrtimer_start(&pulse_reader_dev->pulse_timer,
		ms_to_ktime(pulse_reader_dev->calculate_period), HRTIMER_MODE_REL);

	cdev_init(&pulse_reader_dev->cdev, &pulse_reader_fops);
	pulse_reader_dev->cdev.owner = THIS_MODULE;
	pulse_reader_dev->cdev.ops = &pulse_reader_fops;
	err = cdev_add(&pulse_reader_dev->cdev, devno, 1);
	if (err)
		printk(KERN_ERR "pulse_reader_setup_cdev Error %d adding CDEV%d", err, 0);

	return 0;

fail_malloc:
	device_destroy(pulse_reader_class, devno);
	class_destroy(pulse_reader_class);
fail_create_class:
	unregister_chrdev_region(devno, 1);
	return result;
}

void pulse_reader_exit(void)
{
    dev_t devno = MKDEV(pulse_reader_major, pulse_reader_minor);

    if (pulse_reader_dev) {
        int i;
        hrtimer_cancel(&pulse_reader_dev->pulse_timer);
        for (i = 0; i < MAX_IO_NUMBER; i++) {
            io_stat_t *p_stat = &(pulse_reader_dev->io_stats[i]);
            if (p_stat->used) {
                free_irq(p_stat->irq, pulse_reader_dev);
                gpio_free(p_stat->gpio);
            }
        }

        cdev_del(&pulse_reader_dev->cdev);
        kfree(pulse_reader_dev);
        pulse_reader_dev = NULL;
    }

    if (pulse_reader_class) {
		device_destroy(pulse_reader_class, devno);
        class_destroy(pulse_reader_class);
        pulse_reader_class = NULL;
    }

    unregister_chrdev_region(devno, 1);
}

module_init(pulse_reader_init);
module_exit(pulse_reader_exit);

MODULE_AUTHOR("Nick Liu");
MODULE_LICENSE("GPL");
