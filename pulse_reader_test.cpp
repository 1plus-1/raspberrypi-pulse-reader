/*
	For pulse reader test
 */
 
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/ioctl.h>

typedef unsigned int uint32_t;

#define	ADD_IO				0x7B01
#define	REMOVE_IO			0x7B02
#define	SET_CAL_PERIOD	0x7B03//set period in ms
#define	GET_IO_STAT		0x7B04

#define	MAX_IO_NUMBER	10

#define GPIO_25	25
#define GPIO_26	26

typedef struct
{
	uint32_t gpio;
	uint32_t filter_win_size;
} add_io_t;

typedef struct
{
	uint32_t gpio;
	uint32_t duty;
	uint32_t cycle;
} io_stat_user_t;

typedef struct
{
	io_stat_user_t io_stat_user[MAX_IO_NUMBER];
	uint32_t n_ios;
} get_io_stat_t;


int main(int argc, char **argv)
{
	int fd, i;
	unsigned int period, gpio;
	add_io_t add_io;

	fd = open("/dev/pulse_reader", O_RDWR);
	if (fd < 0) {
		printf("Error open\n");
		return 0;
	}

	if(argc != 2)
		return 0;

	switch(argv[1][0])
	{
	case '0':
		{
			//test set period
			printf("ioctl SET_CAL_PERIOD 10ms\n");
			period = 10;
			if (ioctl(fd, SET_CAL_PERIOD, &period) == -1) {
				close(fd);
				printf("Error ioctl SET_CAL_PERIOD %d\n", period);
				return 0;
			}
		}
		break;
	case '1':
		{
			//test add and remove
			printf("ioctl ADD_IO GPIO_25\n");
			add_io.gpio = GPIO_25;
			add_io.filter_win_size = 5;
			if (ioctl(fd, ADD_IO, &add_io) == -1) {
				close(fd);
				printf("Error ioctl ADD_IO GPIO_25\n");
				return 0;
			}
			printf("ioctl ADD_IO GPIO_26\n");
			add_io.gpio = GPIO_26;
			add_io.filter_win_size = 3;
			if (ioctl(fd, ADD_IO, &add_io) == -1) {
				close(fd);
				printf("Error ioctl ADD_IO GPIO_26\n");
				return 0;
			}
			printf("ioctl REMOVE_IO GPIO_25\n");
			gpio = GPIO_25;
			if (ioctl(fd, REMOVE_IO, &gpio) == -1) {
				close(fd);
				printf("Error ioctl REMOVE_IO GPIO_25\n");
				return 0;
			}
			printf("ioctl REMOVE_IO GPIO_26\n");
			gpio = GPIO_26;
			if (ioctl(fd, REMOVE_IO, &gpio) == -1) {
				close(fd);
				printf("Error ioctl REMOVE_IO GPIO_26\n");
				return 0;
			}
		}
		break;
	case '2':
	{
		//test get stat
		printf("ioctl ADD_IO GPIO_25\n");
		add_io.gpio = GPIO_25;
		add_io.filter_win_size = 5;
		if (ioctl(fd, ADD_IO, &add_io) == -1) {
			close(fd);
			printf("Error ioctl ADD_IO GPIO_25\n");
			return 0;
		}
		printf("ioctl ADD_IO GPIO_26\n");
		add_io.gpio = GPIO_26;
		add_io.filter_win_size = 3;
		if (ioctl(fd, ADD_IO, &add_io) == -1) {
			close(fd);
			printf("Error ioctl ADD_IO GPIO_26\n");
			return 0;
		}
		usleep(500000);
		for(i=0; i<1000; i++) {
			get_io_stat_t io_stat;
			io_stat.io_stat_user[0].gpio = GPIO_25;
			io_stat.io_stat_user[1].gpio = GPIO_26;
			io_stat.n_ios = 2;
			if (ioctl(fd, GET_IO_STAT, &io_stat) == -1) {
				printf("Error ioctl GET_IO_STAT\n");
				return 0;
			}

			printf("GPIO_25 duty = %d, cycle=%d; GPIO_26 duty = %d, cycle=%d; \n",
				io_stat.io_stat_user[0].duty, io_stat.io_stat_user[0].cycle,
				io_stat.io_stat_user[1].duty, io_stat.io_stat_user[1].cycle);
			usleep(20000);
		}
		printf("ioctl REMOVE_IO GPIO_25\n");
		gpio = GPIO_25;
		if (ioctl(fd, REMOVE_IO, &gpio) == -1) {
			close(fd);
			printf("Error ioctl REMOVE_IO GPIO_25\n");
			return 0;
		}
		printf("ioctl REMOVE_IO GPIO_26\n");
		gpio = GPIO_26;
		if (ioctl(fd, REMOVE_IO, &gpio) == -1) {
			close(fd);
			printf("Error ioctl REMOVE_IO GPIO_26\n");
			return 0;
		}
	}
	default:
		break;
	}

	close(fd);
	
	return 0;
}

