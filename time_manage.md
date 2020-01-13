# libevent的时间管理

理解libevent时间管理的关键理清**缓存的系统时间**和**base所拥有的时间**之间的关系.

- 在事件循环开始之前清空缓存时间

- 对于每一次事件循环:
	- 获取时间:
		- 第一次进入这个循环`tv_cache`为空,需要调用系统调用获取时间 否则获得的时间为缓存的时间,也是上一次`poll`完成时的时间
		- 获得的这个时间大于base时间则正常返回 否则需要调整优先队列里的时间

	- 计算超时时间(0/[timer timeout time - gettime()获得的上一次poll()完成时的时间])
	- 赋缓存的时间给`base->event_tv`
	- 清空`base->tv_cache` 下一次`gettime()`不会获得缓存的时间
	- poll
	- 获取系统时间(非缓存的时间) 因此`tv_cache`应该是晚于`event_tv`的 这个在下一轮循环的`timeout_correct()`
		以及接下来处理超时的timer用到
	- 处理超时timer
	- 处理激活或者说完成的事件
	- 判断是否继续循环

## Sum
  定时器与**io multiplexer**整合在了一起,将最先超时的定时器与现在的时间之间的差作为`poll`的`timeout`,这种思路很巧妙,
**libev**和**libuv**也是这样整合事件循环中的**timer**的.
