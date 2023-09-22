#include <nano/lib/asio.hpp>

nano::shared_const_buffer::shared_const_buffer (nano::vectorbuffer const & data) :
	m_data (std::make_shared<nano::vectorbuffer> (data)),
	m_buffer (boost::asio::buffer (*m_data))
{
}

nano::shared_const_buffer::shared_const_buffer (nano::vectorbuffer && data) :
	m_data (std::make_shared<nano::vectorbuffer> (std::move (data))),
	m_buffer (boost::asio::buffer (*m_data))
{
}

//nano::shared_const_buffer::shared_const_buffer (nano::vectorbuffer::value_type data) :
//	shared_const_buffer (nano::vectorbuffer{ data })
//{
//}

nano::shared_const_buffer::shared_const_buffer (std::string const & data) :
	m_data (std::make_shared<nano::vectorbuffer> (data.begin (), data.end ())),
	m_buffer (boost::asio::buffer (*m_data))
{
}

nano::shared_const_buffer::shared_const_buffer (std::shared_ptr<nano::vectorbuffer> const & data) :
	m_data (data),
	m_buffer (boost::asio::buffer (*m_data))
{
}

boost::asio::const_buffer const * nano::shared_const_buffer::begin () const
{
	return &m_buffer;
}

boost::asio::const_buffer const * nano::shared_const_buffer::end () const
{
	return &m_buffer + 1;
}

std::size_t nano::shared_const_buffer::size () const
{
	return m_buffer.size ();
}

nano::vectorbuffer nano::shared_const_buffer::to_bytes () const
{
	nano::vectorbuffer bytes;
	for (auto const & buffer : *this)
	{
		bytes.resize (bytes.size () + buffer.size ());
		std::copy ((uint8_t const *)buffer.data (), (uint8_t const *)buffer.data () + buffer.size (), bytes.data () + bytes.size () - buffer.size ());
	}
	return bytes;
}
