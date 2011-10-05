#ifndef MYSQLXX_POOL_H
#define MYSQLXX_POOL_H

#include <list>

#include <mysql/mysqld_error.h>

#include <Poco/Util/Application.h>
#include <Poco/Util/LayeredConfiguration.h>
#include <Poco/NumberFormatter.h>
#include <Poco/Mutex.h>
#include <Poco/Exception.h>
#include <Poco/SharedPtr.h>

#include <Yandex/logger_useful.h>
#include <Yandex/daemon.h>

#include <Yandex/daemon.h>

#include <mysqlxx/Connection.h>


#define MYSQLXX_POOL_DEFAULT_START_CONNECTIONS 	1
#define MYSQLXX_POOL_DEFAULT_MAX_CONNECTIONS 	16
#define MYSQLXX_POOL_SLEEP_ON_CONNECT_FAIL 		10


namespace mysqlxx
{

/** @brief Пул соединений с MySQL.
  * Этот класс имеет мало отношения в mysqlxx и сделан не в стиле библиотеки. (взят из старого кода)
  * Использование:
  * 	mysqlxx::Pool pool("mysql_params");
  *
  *		void thread()
  *		{
  *		  	mysqlxx::Pool::Entry connection = pool.Get();
  *			std::string s = connection->query("SELECT 'Hello, world!' AS world").use().fetch()["world"].getString();
  *		}
  */
class Pool
{
protected:
	/** @brief Информация о соединении. */
	struct Connection
	{
		Connection() : ref_count(0) {}

		mysqlxx::Connection conn;
		int ref_count;
	};

public:
	/** @brief Соединение с базой данных. */
	class Entry
	{
	public:
		Entry() : data(NULL), pool(NULL) {}

		/** @brief Конструктор копирования. */
		Entry(const Entry & src)
			: data(src.data), pool(src.pool)
		{
			if (data)
				++data->ref_count;
		}

		/** @brief Деструктор. */
		virtual ~Entry()
		{
			if (data)
				--data->ref_count;
		}

		/** @brief Оператор присваивания. */
		Entry & operator= (const Entry & src)
		{
			pool = src.pool;
			if (data)
				--data->ref_count;
			data = src.data;
			if (data)
				++data->ref_count;
			return * this;
		}

		bool isNull() const
		{
			return data == NULL;
		}

		/** @brief Оператор доступа к вложенному объекту. */
		operator mysqlxx::Connection & ()
		{
			if (data == NULL)
				throw Poco::RuntimeException("Tried to access NULL database connection.");
			ForceConnected();
			return data->conn;
		}

		/** @brief Оператор доступа к вложенному объекту. */
		operator const mysqlxx::Connection & () const
		{
			if (data == NULL)
				throw Poco::RuntimeException("Tried to access NULL database connection.");
			ForceConnected();
			return data->conn;
		}

		/** @brief Оператор доступа к вложенному объекту. */
		const mysqlxx::Connection * operator->() const
		{
			if (data == NULL)
				throw Poco::RuntimeException("Tried to access NULL database connection.");
			ForceConnected();
			return &data->conn;
		}

		/** @brief Оператор доступа к вложенному объекту. */
		mysqlxx::Connection * operator->()
		{
			if (data == NULL)
				throw Poco::RuntimeException("Tried to access NULL database connection.");
			ForceConnected();
			return &data->conn;
		}

		/** @brief Конструктор */
		Entry(Pool::Connection * conn, Pool * p)
			: data(conn), pool(p)
		{
			if (data)
				data->ref_count++;
		}

		friend class Pool;

	private:
		/** @brief Указатель на соединение. */
		Connection * data;
		/** @brief Указатель на пул, которому мы принадлежим. */
		Pool * pool;

		/** Переподключается к базе данных в случае необходимости. Если не удалось - подождать и попробовать снова. */
		void ForceConnected() const
		{
			Poco::Util::Application & app = Poco::Util::Application::instance();

			if (data->conn.ping())
				return;

			bool first = true;
			do
			{
				if (first)
					first = false;
				else
					::sleep(5);

				app.logger().information("MYSQL: Reconnecting to " + pool->description);
				data->conn.connect(pool->config_name);
			}
			while (!data->conn.ping());

			pool->afterConnect(data->conn);
		}

		/** Переподключается к базе данных в случае необходимости. Если не удалось - вернуть false. */
		bool tryForceConnected() const
		{
			return data->conn.ping();
		}
	};

	/**
	 * @brief Конструктор.
	 * @param ConfigName		Имя параметра в конфигурационном файле.
	 * @param DefConn			Количество подключений по-умолчанию
	 * @param MaxConn			Максимальное количество подключений
	 * @param AllowMultiQueries	Не используется.
	 */
	Pool(const std::string & config_name_,
		 unsigned DefConn = MYSQLXX_POOL_DEFAULT_START_CONNECTIONS,
		 unsigned MaxConn = MYSQLXX_POOL_DEFAULT_MAX_CONNECTIONS,
			const std::string & InitConnect_ = "")
		: DefaultConnections(DefConn), MaxConnections(MaxConn), InitConnect(InitConnect_),
		Initialized(false), config_name(config_name_), was_successful(false)
	{
	}

	/** @brief Деструктор. */
	~Pool()
	{
		Poco::ScopedLock<Poco::FastMutex> Locker(lock);

		for (ConnList::iterator it = Connections.begin(); it != Connections.end(); it++)
			delete static_cast<Connection *>(*it);
	}

	/** @brief Выделяет соединение для работы. */
	Entry Get()
	{
		Poco::ScopedLock<Poco::FastMutex> Locker(lock);

		Initialize();
		for (;;)
		{
			for (ConnList::iterator it = Connections.begin(); it != Connections.end(); it++)
			{
				if ((*it)->ref_count == 0)
					return Entry(*it, this);
			}

			if (Connections.size() < (size_t)MaxConnections)
			{
				Connection * conn = AllocConnection();
				if (conn)
					return Entry(conn, this);
			}

			lock.unlock();
			sched_yield();
			::sleep(MYSQLXX_POOL_SLEEP_ON_CONNECT_FAIL);
			lock.lock();
		}
	}

	/** Выделяет соединение для работы.
	  * Если база недоступна - возвращает пустой объект Entry.
	  * Если пул переполнен - кидает исключение.
	  */
	Entry tryGet()
	{
		Poco::ScopedLock<Poco::FastMutex> locker(lock);

		Initialize();

		/// Поиск уже установленного, но не использующегося сейчас соединения.
		for (ConnList::iterator it = Connections.begin(); it != Connections.end(); ++it)
		{
			if ((*it)->ref_count == 0)
			{
				Entry res(*it, this);
				return res.tryForceConnected() ? res : Entry();
			}
		}

		/// Если пул переполнен.
		if (Connections.size() >= MaxConnections)
			throw Poco::Exception("mysqlxx::Pool is full");

		/// Выделение нового соединения.
		Connection * conn = AllocConnection(true);
		if (conn)
			return Entry(conn, this);

		return Entry();
	}


	/// Получить описание БД
	std::string getDescription() const
	{
		return description;
	}

protected:
	/** @brief Количество соединений с MySQL, создаваемых при запуске. */
	unsigned DefaultConnections;
	/** @brief Максимально возможное количество соедиений. */
	unsigned MaxConnections;
	/** @brief Запрос, выполняющийся сразу после соединения с БД. Пример: "SET NAMES cp1251". */
	std::string InitConnect;

private:
	/** @brief Признак того, что мы инициализированы. */
	bool Initialized;
	/** @brief Список соединений. */
	typedef std::list<Connection *> ConnList;
	/** @brief Список соединений. */
	ConnList Connections;
	/** @brief Замок для доступа к списку соединений. */
	Poco::FastMutex lock;
	/** @brief Имя раздела в конфигурационном файле. */
	std::string config_name;
	/** @brief Описание соединения. */
	std::string description;

	/** @brief Хотя бы один раз было успешное соединение. */
	bool was_successful;

	/** @brief Выполняет инициализацию класса, если мы еще не инициализированы. */
	inline void Initialize()
	{
		if (!Initialized)
		{
			Poco::Util::Application & app = Poco::Util::Application::instance();
			Poco::Util::LayeredConfiguration & cfg = app.config();

			description = cfg.getString(config_name + ".db", "")
				+ "@" + cfg.getString(config_name + ".host")
				+ ":" + cfg.getString(config_name + ".port")
				+ " as user " + cfg.getString(config_name + ".user");

			for (unsigned i = 0; i < DefaultConnections; i++)
				AllocConnection();

			Initialized = true;
		}
	}

	/** @brief Создает новое соединение. */
	Connection * AllocConnection(bool dont_throw_if_failed_first_time = false)
	{
		Poco::Util::Application & app = Poco::Util::Application::instance();
		Connection * Conn;

		Conn = new Connection();
		try
		{
			app.logger().information("MYSQL: Connecting to " + description);
			Conn->conn.connect(config_name);
		}
		catch (mysqlxx::ConnectionFailed & e)
		{
			if ((!was_successful && !dont_throw_if_failed_first_time)
				|| e.errnum() == ER_ACCESS_DENIED_ERROR
				|| e.errnum() == ER_DBACCESS_DENIED_ERROR
				|| e.errnum() == ER_BAD_DB_ERROR)
			{
				app.logger().error(e.what());
				throw;
			}
			else
			{
				app.logger().error(e.what());
				delete Conn;

				if (Daemon::instance().isCancelled())
					throw Poco::Exception("Daemon is cancelled while trying to connect to MySQL server.");

				return NULL;
			}
		}

		was_successful = true;
		afterConnect(Conn->conn);
		Connections.push_back(Conn);
		return Conn;
	}


	/** @brief Действия, выполняемые после соединения. */
	void afterConnect(mysqlxx::Connection & Conn)
	{
		Poco::Util::Application & app = Poco::Util::Application::instance();

		/// Инициализирующий запрос (например, установка другой кодировки)
		if (!InitConnect.empty())
		{
			mysqlxx::Query Q = Conn.query();
			Q << InitConnect;
			app.logger().trace(Q.str());
			Q.execute();
		}
	}
};

}

#endif
