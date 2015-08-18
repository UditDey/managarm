
namespace frigg {
namespace util {

template<typename T, typename Allocator>
class Vector {
public:
	Vector(Allocator &allocator);
	~Vector();

	T &push(const T &element);
	T &push(T &&element);
	size_t size();
	T &operator[] (size_t index);

private:
	void ensureCapacity(size_t capacity);

	Allocator &p_allocator;
	T *p_elements;
	size_t p_size;
	size_t p_capacity;
};

template<typename T, typename Allocator>
Vector<T, Allocator>::Vector(Allocator &allocator)
		: p_allocator(allocator), p_elements(nullptr), p_size(0), p_capacity(0) { }

template<typename T, typename Allocator>
Vector<T, Allocator>::~Vector() {
	for(size_t i = 0; i < p_size; i++)
		p_elements[i].~T();
	p_allocator.free(p_elements);
}

template<typename T, typename Allocator>
T &Vector<T, Allocator>::push(const T &element) {
	ensureCapacity(p_size + 1);
	p_elements[p_size] = element;
	p_size++;
	return p_elements[p_size - 1];
}

template<typename T, typename Allocator>
T &Vector<T, Allocator>::push(T &&element) {
	ensureCapacity(p_size + 1);
	T *pointer = new (&p_elements[p_size]) T(traits::move(element));
	p_size++;
	return *pointer;
}

template<typename T, typename Allocator>
size_t Vector<T, Allocator>::size() {
	return p_size;
}

template<typename T, typename Allocator>
T &Vector<T, Allocator>::operator[] (size_t index) {
	return p_elements[index];
}

template<typename T, typename Allocator>
void Vector<T, Allocator>::ensureCapacity(size_t capacity) {
	if(capacity <= p_capacity)
		return;
	
	size_t new_capacity = capacity * 2;	
	T *new_array = (T *)p_allocator.allocate(sizeof(T) * new_capacity);
	for(size_t i = 0; i < p_capacity; i++)
		new (&new_array[i]) T(traits::move(p_elements[i]));
	
	for(size_t i = 0; i < p_size; i++)
		p_elements[i].~T();
	p_allocator.free(p_elements);

	p_elements = new_array;
	p_capacity = new_capacity;
}

}} // namespace frigg::util
