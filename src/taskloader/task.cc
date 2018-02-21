#include "task.h"

#include <cstring>
#include <vector>

#include <base/lock.h>

//#include <dom0-HW/dom0_connection.h>

Task::Child_policy::Child_policy(Task& task) :
		_task{&task},
		_labeling_policy{task.name().c_str()},
		_config_policy{"config", task._config.cap(), &task._child_ep},
		_binary_policy{"binary", task._shared.binaries.at(task._desc.binary_name).cap(), &task._child_ep},
		_active{true}
{
}

void Task::Child_policy::exit(int exit_value)
{
	Genode::Lock::Guard guard(_exit_lock);
	// Already exited, waiting for destruction.
	if (!_active)
	{
		return;
	}
	_active = false;
	//PDBG("child %s exited with exit value %d", name(), exit_value);

	Task::Event::Type type;
	switch (exit_value)
	{
		case 0:
			type = Event::EXIT; break;
		case 17:
			type = Event::EXIT_CRITICAL; break;
		case 19:
			type = Event::EXIT_EXTERNAL; break;
		default:
			type = Event::EXIT_ERROR;
	}
	
	Task::log_profile_data(type, _task->_desc.id, _task->_shared);
	if(_task->jobs_done())
	{
		type=Task::Event::JOBS_DONE;
                Task::log_profile_data(type, _task->_desc.id, _task->_shared);
	}
	Dom0_server::Connection dom0;
	dom0.send_profile(name());
	Task::_child_destructor.submit_for_destruction(_task);
	
	


	
}

const char* Task::Child_policy::name() const
{
	return _task->name().c_str();
}

bool Task::Child_policy::active() const
{
	return _active;
}

Genode::Service* Task::Child_policy::resolve_session_request(const char* service_name, const char* args)
{
	Genode::Service* service = nullptr;

	// Check for config file request.
	if ((service = _config_policy.resolve_session_request(service_name, args)))
	{
		return service;
	}

	// Check for binary file request.
	if ((service = _binary_policy.resolve_session_request(service_name, args)))
	{
		return service;
	}

	// Check child services.
	if ((service = _task->_shared.child_services.find(service_name)))
	{
		return service;
	}

	// Check parent services.
	if ((service = _task->_shared.parent_services.find(service_name)))
	{
		return service;
	}

	PINF("Service %s requested by %s not found. Waiting for it to become available.", service_name, name());

	Genode::Client client;
	return _task->_shared.child_services.wait_for_service(service_name, &client, name());
}

void Task::Child_policy::filter_session_args(const char *service, char *args, Genode::size_t args_len)
{
	_labeling_policy.filter_session_args(service, args, args_len);
}

bool Task::Child_policy::announce_service(
	const char *service_name,
	Genode::Root_capability root,
	Genode::Allocator *alloc,
	Genode::Server*)
{
	if (_task->_shared.child_services.find(service_name)) {
		PWRN("%s: service %s is already registered", name(), service_name);
		return false;
	}

	_task->_shared.child_services.insert(new (alloc) Genode::Child_service(service_name, root, &_task->_meta->server));
	PINF("%s registered service %s\n", name(), service_name);

	return true;
}

void Task::Child_policy::unregister_services()
{
	Genode::Service *rs;
	while ((rs = _task->_shared.child_services.find_by_server(&_task->_meta->server)))
	{
		_task->_shared.child_services.remove(rs);
	}
}



Task::Meta::Meta(const Task& task) :
	ram{},
	cpu{task.name().c_str(), (128-task._desc.priority)*(Genode::Cpu_session::PRIORITY_LIMIT >> Genode::log2(128)), task._desc.deadline*1000, Genode::Affinity(Genode::Affinity::Space(_mon.get_num_cores(),1), Genode::Affinity::Location(1,0))},
	rm{},
	pd{},
	server{ram}
{
	ram.ref_account(Genode::env()->ram_session_cap());
	if (Genode::env()->ram_session()->transfer_quota(ram.cap(), task._desc.quota) != 0)
	{
		PWRN("Failed to transfer RAM quota to child %s", task.name().c_str());
	}
}



Task::Meta_ex::Meta_ex(Task& task) :
		Meta{task},
		policy{task},
		_initial_thread(cpu, pd.cap(), task.name().c_str()), //task._desc.binary_name.c_str()),
		ldso_rom{"ld.lib.so"},
		rmc(pd.address_space()),
		child {
			task._shared.binaries.at(task._desc.binary_name).cap(),
			ldso_rom.dataspace(),
			pd.cap(),
			pd,
			ram.cap(),
			ram,
			cpu.cap(),
			_initial_thread,
			*Genode::env()->rm_session(),
			rmc,
			task._child_ep,
			policy
		}
{
}



const char* Task::Event::type_name(Type type)
{
	switch (type)
	{
		case START: return "START";
		case EXIT: return "EXIT";
		case EXIT_CRITICAL: return "EXIT_CRITICAL";
		case EXIT_ERROR: return "EXIT_ERROR";
		case EXIT_EXTERNAL: return "EXIT_EXTERNAL";
		case EXTERNAL: return "EXTERNAL";
		default: return "UNKNOWN";
	}
}



Task::Shared_data::Shared_data(size_t trace_quota, size_t trace_buf_size) :
	binaries{},
	heap{Genode::env()->ram_session(), Genode::env()->rm_session()},
	parent_services{},
	trace{trace_quota, trace_buf_size, 0}
{
}



Task::Task(Server::Entrypoint& ep, Genode::Cap_connection& cap, Shared_data& shared, const Genode::Xml_node& node, Sched_controller::Connection* ctrl) :
		_shared(shared),
		_desc{
			_get_node_value<unsigned int>(node, "id"),
			_get_node_value<unsigned int>(node, "executiontime"),
			_get_node_value<unsigned int>(node, "criticaltime"),
			_get_node_value<unsigned long>(node, "priority"),
			_get_node_value<unsigned int>(node, "deadline"),
			_get_node_value<unsigned int>(node, "period"),
			_get_node_value<unsigned int>(node, "offset"),
			_get_node_value<unsigned int>(node, "numberofjobs"),
			_get_node_value<Genode::Number_of_bytes>(node, "quota"),
			_get_node_value(node, "pkg", 32, "")},
		_config{Genode::env()->ram_session(), node.sub_node("config").size()},
		_name{_make_name()},
		_iteration{0},
		_paused{true},
		_start_timer{},
		_kill_timer{},
		_start_dispatcher{ep, *this, &Task::_start},
		_kill_dispatcher{ep, *this, &Task::_kill_crit},
		_idle_dispatcher{ep, *this, &Task::_idle},
		_child_ep{&cap, 12 * 1024, _name.c_str(), false},
		_meta{nullptr},
		_controller(ctrl),
		_schedulable(true)
{
	const Genode::Xml_node& config_node = node.sub_node("config");
	std::strncpy(_config.local_addr<char>(), config_node.addr(), config_node.size());
	PINF("id: %u, name: %s, prio: %u, deadline: %u, wcet: %u, period: %u", _desc.id, _name.c_str(), _desc.priority, _desc.deadline, _desc.execution_time, _desc.period);
}

Task::~Task()
{
}

void Task::setSchedulable(bool schedulable)
{
	_schedulable = schedulable;
}

bool Task::isSchedulable()
{
	return _schedulable;
}

unsigned int Task::get_id()
{
	return _desc.id;
}

Task::Shared_data& Task::get_shared()
{
	return _shared;
}

bool Task::jobs_done()
{
	PDBG("iteration: %d num jobs: %d name: %s\n", _iteration, _desc.number_of_jobs, _name.c_str());
	return _iteration==_desc.number_of_jobs;

}

Rq_task::Rq_task Task::getRqTask()
{
	Rq_task::Rq_task rq_task;
	rq_task.task_id = _desc.id;
	rq_task.wcet = _desc.execution_time;
	rq_task.prio = _desc.priority;
	rq_task.inter_arrival = _desc.period;
	rq_task.deadline = _desc.deadline*1000;
	strcpy(rq_task.name, _name.c_str());
	
	if(_desc.deadline > 0)
	{
		rq_task.task_class = Rq_task::Task_class::lo;
		rq_task.task_strategy = Rq_task::Task_strategy::deadline;
	}
	else
	{
		rq_task.task_class = Rq_task::Task_class::hi;
		rq_task.task_strategy = Rq_task::Task_strategy::priority;
	}
	
	return rq_task;
}

void Task::run()
{
	_paused = false;

	// (Re-)Register timeout handlers.
	Timer::Connection offset_timer;
	//PDBG("Waiting %dms",_desc.offset);
	offset_timer.msleep(_desc.offset);
	
	_start_timer.sigh(_start_dispatcher);
	_kill_timer.sigh(_kill_dispatcher);

	if (_desc.period > 0)
	{
		_start_timer.trigger_periodic(_desc.period * 1000);
	}
	else
	{
		_start(0);
	}
}

void Task::stop()
{
	PINF("Stopping task %s\n", _name.c_str());
	_paused = true;
	_stop_timers();
	_kill(19);
}

std::string Task::name() const
{
	return _name;
}

bool Task::running() const
{
	return _meta != nullptr;
}

const Task::Description& Task::desc() const
{
	return _desc;
}

Task* Task::task_by_name(std::list<Task>& tasks, const std::string& name)
{
	for (Task& task : tasks)
	{
		if (task.name() == name)
		{
			return &task;
		}
	}
	return nullptr;
}

void Task::log_profile_data(Event::Type type, int task_id, Shared_data& shared)
{
	static const size_t MAX_NUM_SUBJECTS = 128;
	// Lock to avoid race conditions as this may be called by the child's thread.
	Genode::Lock::Guard guard(shared.log_lock);

	Genode::Trace::Subject_id subjects[MAX_NUM_SUBJECTS];
	const size_t num_subjects = shared.trace.subjects(subjects, MAX_NUM_SUBJECTS);
	Genode::Trace::CPU_info info;
	Genode::Trace::RAM_info ram_info;

	shared.event_log.emplace_back();
	Event& event = shared.event_log.back();

	event.type = type;
	event.task_id = task_id;
	event.time_stamp = shared.timer.now_us()/1000;

	Event::Task_info* task_manager_info = nullptr;

	for (Genode::Trace::Subject_id* subject = subjects; subject < subjects + num_subjects; ++subject)
	{
		info = shared.trace.cpu_info(*subject);
		ram_info = shared.trace.ram_info(*subject);
		event.task_infos.emplace_back();
		Event::Task_info& task_info = event.task_infos.back();

		task_info.id = subject->id;
		task_info.session = ram_info.session_label().string(),
		task_info.thread = ram_info.thread_name().string(),
		task_info.state = info.state(),
		task_info.execution_time = info.execution_time().value;

		// Check if the session is started by this task manager (i.e., a managed task).
		size_t leaf_pos = task_info.session.rfind("task-manager -> ");
		Task* task = nullptr;
		if (leaf_pos < std::string::npos)
		{
			const std::string process = task_info.session.substr(leaf_pos + 16);
			if (process == task_info.thread)
			{
				task = task_by_name(shared.tasks, task_info.session.substr(leaf_pos + 16));
			}
		}
		if (task && task->running())
		{
			task_info.managed = true;
			task_info.managed_info.id = task->_desc.id;
			task_info.managed_info.quota = task->_meta->ram.quota();
			task_info.managed_info.used = task->_meta->ram.used();
			task_info.managed_info.iteration = task->_iteration;
		}
		// Check if this is task-manager itself.
		else if (task_info.session.rfind("task-manager") == task_info.session.length() - 12 && task_info.thread == "task-manager")
		{
			task_info.managed = true;
			task_info.managed_info.id = 0;
			task_info.managed_info.quota = Genode::env()->ram_session()->quota();
			task_info.managed_info.used = Genode::env()->ram_session()->used();
			task_info.managed_info.iteration = 0;

			// Hack: there are two task-manager processes. We only flag the more active one as managed.
			if (!task_manager_info)
			{
				task_manager_info = &task_info;
			}
			else if (task_manager_info->execution_time < task_info.execution_time)
			{
				task_manager_info->managed = false;
			}
		}
	}
}

std::string Task::_make_name() const
{
	char id[4];
	snprintf(id, sizeof(id), "%.2d.", _desc.id);
	return std::string(id) + _desc.binary_name;
	// return _desc.binary_name;
}

void Task::_start(unsigned)
{
	if (jobs_done())
	{
		//trigger optimization to let all remaining tasks finish running
		_controller->scheduling_allowed(_name.c_str());
		PINF("%s JOBS DONE!", _name.c_str());
		return;
	}
	if(_desc.deadline>0)
	{
		if(!_controller->scheduling_allowed(_name.c_str()))
		{
			PINF("%s NOT ALLOWED!", _name.c_str());
			Task::Event::Type type=Task::Event::NOT_SCHEDULED;
			Task::log_profile_data(type, get_id(), get_shared());
			return;
		}
		PINF("%s ALLOWED!", _name.c_str());
	}
	if (_paused)
	{
		// This might happen if start timeout is triggered before a stop call but is handled after.
		return;
	}

	if (running())
	{
		PINF("Trying to start %s but previous instance still running or undestroyed. Abort.\n", _name.c_str());
		Task::_child_destructor.submit_for_destruction(this);
		return;
	}

	// Check if binary has already been received.
	auto bin_it = _shared.binaries.find(_desc.binary_name);
	if (bin_it == _shared.binaries.end())
	{
		PERR("Binary %s for task %s not found, possibly not yet received by dom0.", _desc.binary_name.c_str(), _name.c_str());
		return;
	}

	Genode::Attached_ram_dataspace& ds = bin_it->second;

	++_iteration;
	//PINF("Starting task %s with quota %u and priority %u in iteration %d", _name.c_str(), (size_t)_desc.quota, _desc.priority, _iteration);

	if ((size_t)_desc.quota < 512 * 1024)
	{
		PWRN("Warning: RAM quota for %s might be too low to hold meta data.", _name.c_str());
	}

	// Dispatch kill timer after critical time.
	if (_desc.critical_time > 0)
	{
		_kill_timer.trigger_once(_desc.critical_time * 1000);
	}

	// Abort if RAM quota insufficient. Alternatively, we could give all remaining quota to the child.
	if (_desc.quota > Genode::env()->ram_session()->avail()) {
		PERR("Not enough RAM quota for task %s, requested: %u, available: %u", _name.c_str(), (size_t)_desc.quota, Genode::env()->ram_session()->avail());
		return;
	}

	try
	{
		// Create child and activate entrypoint.
		_meta = new (&_shared.heap) Meta_ex(*this);
		_child_ep.activate();
	}
	catch (Genode::Cpu_session::Thread_creation_failed)
	{
		PWRN("Failed to create child - Cpu_session::Thread_creation_failed");
	}
	catch (...)
	{
		PWRN("Failed to create child - unknown reason");
	}

	log_profile_data(Event::START, _desc.id, _shared);
}

Task::Child_destructor_thread::Child_destructor_thread() :
	Thread_deprecated{"child_destructor"},
	_lock{},
	_queued{}
{
	start();
}

Task::Child_start_thread::Child_start_thread() :
	Thread_deprecated{"child_start"},
	_lock{},
	_queued{}
{
	start();
}

void Task::Child_destructor_thread::submit_for_destruction(Task* task)
{
	_lock.lock();
	_queued.push_back(task);
	_lock.unlock();
}

void Task::Child_start_thread::submit_for_start(Task* task)
{
	Genode::Lock::Guard guard(_lock);
	_queued.push_back(task);
}

void Task::Child_destructor_thread::entry()
{
	while (true)
	{
		
		_lock.lock();
		if(!_queued.empty())
		{
			Task* task=_queued.front();
			_queued.remove(task);
			_lock.unlock();
			//_timer.msleep(100);
			PDBG("Destroying task %s", task->_name.c_str());
			int time_before=_timer.elapsed_ms();
			Genode::destroy(task->_shared.heap, task->_meta);
			task->_meta = nullptr;
			PDBG("Done Destruction. Took: %d",_timer.elapsed_ms()-time_before);
		}
		_lock.unlock();
		
		
	}
}

void Task::Child_start_thread::entry()
{
	while (true)
	{
		_lock.lock();
		for (Task* task : _queued)
		{
			PDBG("Starting task %s", task->_name.c_str());
			task->run();
		}
		_queued.clear();
		_lock.unlock();
	}
}

Task::Child_destructor_thread Task::_child_destructor;
Task::Child_start_thread Task::_child_start;
Mon_manager::Connection Task::_mon;

void Task::_kill_crit(unsigned)
{
	// Check for paused status for the rare case where timer signals have been triggered before stopping but are handled after.
	if (!_paused)
	{
		PINF("Critical time reached for %s", _name.c_str());
		_kill(17);
	}
}

void Task::_kill(int exit_value)
{
	// Task might have a valid _meta and be inactive for the short time between submitting the task for destruction and the actual destruction. In that case we do nothing.
	if (_meta && _meta->policy.active())
	{
		PINF("Force-exiting %s", _name.c_str());
		// Child::exit() is usually called from the child thread. Use this carefully.
		_meta->child.exit(exit_value);
	}
}

void Task::_idle(unsigned)
{
	// Do nothing.
}

void Task::_stop_timers()
{
	// "Stop" timers. Apparently there is no way to stop a running timer, so instead we let it trigger an idle method.
	_stop_kill_timer();
	_stop_start_timer();
}

void Task::_stop_kill_timer()
{
	_kill_timer.sigh(_idle_dispatcher);
	_kill_timer.trigger_once(0);
}

void Task::_stop_start_timer()
{
	_start_timer.sigh(_idle_dispatcher);
	_start_timer.trigger_once(0);
}

std::string Task::_get_node_value(const Genode::Xml_node& config_node, const char* type, size_t max_len, const std::string& default_val)
{
	if (config_node.has_sub_node(type))
	{
		std::vector<char> out(max_len);
		config_node.sub_node(type).value(out.data(), max_len);
		return out.data();
	}
	return default_val;
}
