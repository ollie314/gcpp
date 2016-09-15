
/////////////////////////////////////////////////////////////////////////////// 
// 
// Copyright (c) 2016 Herb Sutter. All rights reserved. 
// 
// This code is licensed under the MIT License (MIT). 
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR 
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE 
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, 
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN 
// THE SOFTWARE. 
// 
///////////////////////////////////////////////////////////////////////////////


#ifndef GALLOC_DEFERRED_HEAP
#define GALLOC_DEFERRED_HEAP

#include "gpage.h"

#include <vector>
#include <list>
#include <unordered_set>
#include <algorithm>
#include <type_traits>
#include <memory>

namespace galloc {

	//  destructor contains a pointer and type-correct-but-erased dtor call.
	//  (Happily, a noncapturing lambda decays to a function pointer, which
	//	will make these both easy to construct and cheap to store without
	//	resorting to the usual type-erasure machinery.)
	//
	class destructors {
		struct destructor {
			const byte* p;
			std::size_t size;
			std::size_t n;
			void(*destroy)(const void*);
		};
		std::vector<destructor>	dtors;

	public:
		//	Store the destructor, if it's not trivial
		//
		template<class T>
		void store(T* p, std::size_t num = 1) {
			assert(p != nullptr && num > 0
				&& "no object to register for destruction");
			if (!std::is_trivially_destructible<T>::value) {
				dtors.push_back({
					(byte*)p,								// address p
					sizeof(T),
					num,
					[](const void* x) { ((T*)x)->~T(); }	// dtor to invoke with p
				});
			}
		}

		//	Inquire whether there is a destructor registered for p
		//
		template<class T>
		bool is_stored(T* p) noexcept {
			return std::is_trivially_destructible<T>::value
				|| std::find_if(dtors.begin(), dtors.end(), 
					[=](auto x) { return x.p == (byte*)p; }) != dtors.end();
		}

		//	Run all the destructors and clear the list
		//
		void run_all() {
			for (auto& d : dtors) {
				for (std::size_t i = 0; i < d.n; ++i) {
					d.destroy(d.p + d.size*i);	// call object's destructor
				}
			}
			dtors.clear();	// this should just be an assignment to the vector's #used count
		}					// since all the contained objects are trivially destructible

		//	Runn all the destructors for objects in [begin,end)
		//
		bool run(byte* begin, byte* end) {
			assert(begin < end && "begin must precede end");
			bool ret = false;

			//	for reentrancy safety, we'll take a local copy of destructors to be run
			//
			//	first, move any destructors for objects in this range to a local list...
			//
			std::vector<destructor> to_destroy;
			for (auto it = dtors.begin(); it != dtors.end(); /*--*/) {
				if (begin <= it->p && it->p < end) {
					to_destroy.push_back(*it);
					it = dtors.erase(it);
					ret = true;
				}
				else {
					++it;
				}
			}

			//	... then, execute them now that we're done using private state
			//
			for (auto& d : to_destroy) {
				for (std::size_t i = 0; i < d.n; ++i) {
					//	=====================================================================
					//  === BEGIN REENTRANCY-SAFE: ensure no in-progress use of private state
					d.destroy(d.p + d.size*i);	// call object's destructor
					//  === END REENTRANCY-SAFE: reload any stored copies of private state
					//	=====================================================================
				}
			}

			//	else there wasn't a nontrivial destructor
			return ret;
		}

		void debug_print() const;
	};


	//----------------------------------------------------------------------------
	//
	//	The deferred heap produces deferred_ptr<T>s via make<T>.
	//
	//----------------------------------------------------------------------------

	class deferred_heap {
		friend deferred_heap& global_deferred_heap();	// TODO

		class  deferred_ptr_void;
		friend class deferred_ptr_void;

		template<class T> friend class deferred_ptr;
		template<class T> friend class deferred_allocator;

		//	TODO Can only be used via the global global_deferred_heap() accessor.
		deferred_heap()  			  = default;
		~deferred_heap();

		//	Disable copy and move
		deferred_heap(deferred_heap&)		  = delete;
		void operator=(deferred_heap&) = delete;

		//	Add/remove a deferred_ptr in the tracking list.
		//	Invoked when constructing and destroying a deferred_ptr.
		void enregister(const deferred_ptr_void& p);
		void deregister(const deferred_ptr_void& p);

		//------------------------------------------------------------------------
		//
		//  deferred_ptr_void is the generic pointer type we use and track
		//  internally. The user uses deferred_ptr<T>, the type-casting wrapper.
		//
		class deferred_ptr_void {
			void* p;

		protected:
			void  set(void* p_) noexcept { p = p_; }

			deferred_ptr_void(void* p_ = nullptr)
				: p(p_)
			{
				global_deferred_heap().enregister(*this);
			}

			~deferred_ptr_void() {
				global_deferred_heap().deregister(*this);
			}

			deferred_ptr_void(const deferred_ptr_void& that)
				: deferred_ptr_void(that.p)
			{ }

			//	Note: =default makes this assignment operator trivial.
			deferred_ptr_void& operator=(const deferred_ptr_void& that) noexcept = default;
		public:
			void* get() const noexcept { return p; }
			void  reset() noexcept { p = nullptr; }
		};

		//	For non-roots (deferred_ptrs that are in the deferred heap), we'll additionally
		//	store an int that we'll use for terminating marking within the deferred heap.
		//  The level is the distance from some root -- not necessarily the smallest
		//	distance from a root, just along whatever path we took during marking.
		//
		struct nonroot {
			const deferred_ptr_void* p;
			std::size_t level = 0;

			nonroot(const deferred_ptr_void* p_) noexcept : p{ p_ } { }
		};

		struct dhpage {
			gpage				 page;
			bitflags		 	 live_starts;	// for tracing
			std::vector<nonroot> deferred_ptrs;		// known deferred_ptrs in this page

			//	Construct a page tuned to hold Hint objects, big enough for
			//	at least 1 + phi ~= 2.62 of these requests (but at least 64K),
			//	and a tracking min_alloc chunk sizeof(request) (but at least 4 bytes).
			//	Note: Hint used only to deduce size/alignment.
			//	TODO: don't allocate objects on pages with chunk sizes > 2 * object size
			//
			template<class Hint>
			dhpage(const Hint* /*--*/, size_t n)
				: page{ std::max<size_t>(sizeof(Hint) * n * 2.62, 4096 /*good general default*/), 
						std::max<size_t>(sizeof(Hint), 4) }
				, live_starts(page.locations(), false)
			{ }
		};


		//------------------------------------------------------------------------
		//	Data: Storage and tracking information
		//
		std::list<dhpage>						pages;
		std::unordered_set<const deferred_ptr_void*>	roots;	// outside deferred heap
		destructors								dtors;
		bool is_destroying = false;
		bool collect_before_expand = false;

	public:
		//------------------------------------------------------------------------
		//
		//	make: Allocate one object of type T initialized with args
		//
		//	If allocation fails, the returned pointer will be null
		//
		template<class T, class ...Args>
		deferred_ptr<T> make(Args&&... args) {
			auto p = allocate<T>();
			if (p != nullptr) {
				construct(p.get(), std::forward<Args>(args)...);
			}
			return p;
		}

		//------------------------------------------------------------------------
		//
		//	make_array: Allocate n default-constructed objects of type T
		//
		//	If allocation fails, the returned pointer will be null
		//
		template<class T>
		deferred_ptr<T> make_array(std::size_t n) {
			auto p = allocate<T>(n);
			if (p != nullptr) {
				construct_array(p.get(), n);
			}
			return p;
		}

	private:
		//------------------------------------------------------------------------
		//
		//	Core allocator functions: allocate, construct, destroy
		//	(not deallocate, which is done at collection time)
		//
		//	There are private, for use via deferred_allocator only

		//  Helper: Return the dhpage on which this object exists.
		//	If the object is not in our storage, returns null.
		//
		template<class T>
		dhpage* find_dhpage_of(T* p) noexcept;

		struct find_dhpage_info_ret {
			dhpage* page = nullptr;
			gpage::contains_info_ret info;
		};
		template<class T> 
		find_dhpage_info_ret find_dhpage_info(T* p) noexcept;

		template<class T>
		T* allocate_from_existing_pages(std::size_t n);
			
		template<class T>
		deferred_ptr<T> allocate(std::size_t n = 1);

		template<class T, class ...Args> 
		void construct(T* p, Args&& ...args);

		template<class T> 
		void construct_array(T* p, std::size_t n);

		template<class T> 
		void destroy(T* p) noexcept;
		
		bool destroy_objects(byte* start, byte* end);

		//------------------------------------------------------------------------
		//
		//	collect, et al.: Sweep the deferred heap
		//
		void mark(const void* p, std::size_t level) noexcept;

	public:
		void collect();

		auto get_collect_before_expand() {
			return collect_before_expand;
		}

		void set_collect_before_expand(bool enable = false) {
			collect_before_expand = enable;
		}

		void debug_print() const;

	};


	//------------------------------------------------------------------------
	//
	//  deferred_ptr<T> is the typed pointer type for callers to use.
	//
	//------------------------------------------------------------------------
	//
	template<class T>
	class deferred_ptr : public deferred_heap::deferred_ptr_void {
		deferred_ptr(T* p)
			: deferred_ptr_void(p)
		{ }

		friend deferred_heap;

	public:
		//	Default and null construction. (Note we do not use a defaulted
		//	T* parameter, so that the T* overload can be private and the
		//	nullptr overload can be public.)
		//
		deferred_ptr() : deferred_ptr_void(nullptr) { }

		//	Construction and assignment from null. Note: The null constructor
		//	is not defined as a combination default constructor in the usual
		//	way (that is, as constructor from T* with a default null argument)
		//	because general construction from T* is private.
		//
		deferred_ptr(std::nullptr_t) : deferred_ptr{} { }

		deferred_ptr& operator=(std::nullptr_t) noexcept {
			reset();
			return *this;
		}

		//	Copying.
		//
		deferred_ptr(const deferred_ptr& that)
			: deferred_ptr_void(that)
		{ }

		deferred_ptr& operator=(const deferred_ptr& that) noexcept = default;	// trivial copy assignment

		//	Copying with conversions (base -> derived, non-const -> const).
		//
		template<class U>
		deferred_ptr(const deferred_ptr<U>& that)
			: deferred_ptr_void(static_cast<T*>((U*)(that.p)))			// ensure U* converts to T*
		{ }

		template<class U>
		deferred_ptr& operator=(const deferred_ptr<U>& that) noexcept {
			deferred_ptr_void::operator=(static_cast<T*>((U*)that.p));	// ensure U* converts to T*
			return *this;
		}

		//	Accessors.
		//
		T* get() const noexcept {
			return (T*)deferred_ptr_void::get();
		}

		T& operator*() const noexcept {
			//	This assertion is currently disabled because MSVC's std::vector
			//	implementation relies on being able to innocuously dereference
			//	any pointer (even null) briefly just to take the pointee's
			//	address again, to un-fancy "fancy" pointers like this one
			//	(The next VS "15" Preview has a fix & we can re-enable this.)
			//assert(get() && "attempt to dereference null");
			return *get();
		}

		T* operator->() const noexcept {
			assert(get() && "attempt to dereference null");
			return get();
		}

		template<class T>
		static deferred_ptr<T> pointer_to(T& t) {
			return deferred_ptr<T>(&t);
		}


		// this is the right way to do totally ordered comparisons, maybe someday it'll be standard
		int compare3(const deferred_ptr& that) const { return get() < that.get() ? -1 : get() == that.get() ? 0 : 1; };
		GALLOC_TOTALLY_ORDERED_COMPARISON(deferred_ptr);	// maybe someday this will be default
													
													
		//	Checked pointer arithmetic -- TODO this should probably go into a separate array_deferred_ptr type
		//
		deferred_ptr& operator+=(int offset) noexcept {
#ifndef NDEBUG
			assert(get() != nullptr 
				&& "bad deferred_ptr arithmetic: can't perform arithmetic on a null pointer");

			auto this_info = global_deferred_heap().find_dhpage_info(get());

			assert(this_info.page != nullptr
				&& "corrupt non-null deferred_ptr, not pointing into deferred heap");

			assert(this_info.info.found > gpage::in_range_unallocated
				&& "corrupt non-null deferred_ptr, pointing to unallocated memory");

			auto temp = get() + offset;
			auto temp_info = global_deferred_heap().find_dhpage_info(temp);

			assert(this_info.page == temp_info.page 
				&& "bad deferred_ptr arithmetic: attempt to leave dhpage");

			assert(
				//	if this points to the start of an allocation, it's always legal
				//	to form a pointer to the following element (just don't deref it)
				//	which covers one-past-the-end of single-element allocations
				(	(
					this_info.info.found == gpage::in_range_allocated_start
					&& (offset == 0 || offset == 1)
					)
				//	otherwise this and temp must point into the same allocation
				//	which is covered for arrays by the extra byte we allocated
				||	(
					this_info.info.start_location == temp_info.info.start_location 
					&& temp_info.info.found > gpage::in_range_unallocated)
					)
				&& "bad deferred_ptr arithmetic: attempt to go outside the allocation");
#endif
			set(get() + offset);
			return *this;
		}

		deferred_ptr& operator-=(int offset) noexcept {
			return operator+=(-offset);
		}

		deferred_ptr& operator++() noexcept {
			return operator+=(1);
		}

		deferred_ptr& operator++(int) noexcept {
			return operator+=(1);
		}

		deferred_ptr& operator--() noexcept {
			return operator+=(-1);
		}

		deferred_ptr operator+(int offset) const noexcept {
			auto ret = *this;
			ret += offset;
			return ret;
		}

		deferred_ptr operator-(int offset) const noexcept {
			return *this + -offset;
		}

		T& operator[](size_t offset) noexcept {
#ifndef NDEBUG
			//	In debug mode, perform the arithmetic checks by creating a temporary deferred_ptr
			auto tmp = *this;
			tmp += offset;	
			return *tmp;
#else
			//	In release mode, don't enregister/deregister a temnporary deferred_ptr
			return *(get() + offset);
#endif
		}

		ptrdiff_t operator-(const deferred_ptr& that) const noexcept {
#ifndef NDEBUG
			//	Note that this intentionally permits subtracting two null pointers
			if (get() == that.get()) {
				return 0;
			}

			assert(get() != nullptr && that.get() != nullptr
				&& "bad deferred_ptr arithmetic: can't subtract pointers when one is null");

			auto this_info = global_deferred_heap().find_dhpage_info(get());
			auto that_info = global_deferred_heap().find_dhpage_info(that.get());

			assert(this_info.page != nullptr
				&& that_info.page != nullptr
				&& "corrupt non-null deferred_ptr, not pointing into deferred heap");

			assert(that_info.info.found > gpage::in_range_unallocated
				&& "corrupt non-null deferred_ptr, pointing to unallocated space");

			assert(that_info.page == this_info.page
				&& "bad deferred_ptr arithmetic: attempt to leave dhpage");

			assert(
				//	if that points to the start of an allocation, it's always legal
				//	to form a pointer to the following element (just don't deref it)
				//	which covers one-past-the-end of single-element allocations
				//
				//	TODO: we could eliminate this first test by adding an extra byte
				//	to every allocation, then we'd be type-safe too (this being the
				//	only way to form a deferred_ptr<T> to something not allocated as a T)
				((
					that_info.info.found == gpage::in_range_allocated_start
					&& (get() == that.get()+1)
					)
					//	otherwise this and temp must point into the same allocation
					//	which is covered for arrays by the extra byte we allocated
					|| (
						that_info.info.start_location == this_info.info.start_location
						&& this_info.info.found > gpage::in_range_unallocated)
					)
				&& "bad deferred_ptr arithmetic: attempt to go outside the allocation");
#endif

			return get() - that.get();
		}
	};

	//	Specialize void just to get rid of the void& return from op*

	//	TODO actually we should be able to just specialize that one function
	//	(if we do that, also disable arithmetic on void... perhaps that just falls out)

	template<>
	class deferred_ptr<void> : public deferred_heap::deferred_ptr_void {
		deferred_ptr(void* p)
			: deferred_ptr_void(p)
		{ }

		friend deferred_heap;

	public:
		//	Default and null construction. (Note we do not use a defaulted
		//	T* parameter, so that the T* overload can be private and the
		//	nullptr overload can be public.)
		//
		deferred_ptr() : deferred_ptr_void(nullptr) { }

		//	Construction and assignment from null. Note: The null constructor
		//	is not defined as a combination default constructor in the usual
		//	way (that is, as constructor from T* with a default null argument)
		//	because general construction from T* is private.
		//
		deferred_ptr(std::nullptr_t) : deferred_ptr{} { }

		deferred_ptr& operator=(std::nullptr_t) noexcept {
			reset();
			return *this;
		}

		//	Copying.
		//
		deferred_ptr(const deferred_ptr& that)
			: deferred_ptr_void(that)
		{ }

		deferred_ptr& operator=(const deferred_ptr& that) noexcept
		{
			deferred_ptr_void::operator=(that);
			return *this;
		}

		//	Copying with conversions (base -> derived, non-const -> const).
		//
		template<class U>
		deferred_ptr(const deferred_ptr<U>& that)
			: deferred_ptr_void(that)
		{ }

		template<class U>
		deferred_ptr& operator=(const deferred_ptr<U>& that) noexcept {
			deferred_ptr_void::operator=(that);
			return *this;
		}

		//	Accessors.
		//
		void* get() const noexcept {
			return deferred_ptr_void::get(); 
		}

		void* operator->() const noexcept {
			assert(get() && "attempt to dereference null"); 
			return get(); 
		}
	};


	//	Allocate one object of type T initialized with args
	//
	template<class T, class ...Args>
	deferred_ptr<T> make_deferred(Args&&... args) {
		return global_deferred_heap().make<T>( std::forward<Args>(args)... );
	}

	//	Allocate an array of n objects of type T
	//
	template<class T>
	deferred_ptr<T> make_deferred_array(std::size_t n) {
		return global_deferred_heap().make_array<T>(n);
	}


	//----------------------------------------------------------------------------
	//
	//	deferred_heap function implementations
	//
	//----------------------------------------------------------------------------
	//
	inline
	deferred_heap::~deferred_heap() 
	{
		//	Note: setting this flag lets us skip worrying about reentrancy;
		//	a destructor may not allocate a new object (which would try to
		//	enregister and therefore change our data structurs)
		is_destroying = true;

		//	when destroying the arena, reset all pointers and run all destructors 
		//
		for (auto& p : roots) {
			const_cast<deferred_ptr_void*>(p)->reset();
		}

		for (auto& pg : pages) {
			for (auto& p : pg.deferred_ptrs) {
				const_cast<deferred_ptr_void*>(p.p)->reset();
			}
		}

		//	this calls user code (the dtors), but no reentrancy care is 
		//	necessary per note above
		dtors.run_all();
	}

	//	Add this deferred_ptr to the tracking list. Invoked when constructing a deferred_ptr.
	//
	inline
	void deferred_heap::enregister(const deferred_ptr_void& p) {
		//	append it to the back of the appropriate list
		assert(!is_destroying 
			&& "cannot allocate new objects on a deferred_heap that is being destroyed");
		auto pg = find_dhpage_of(&p);
		if (pg != nullptr) 
		{
			pg->deferred_ptrs.push_back(&p);
		}
		else 
		{
			roots.insert(&p);
		}
	}

	//	Remove this deferred_ptr from tracking. Invoked when destroying a deferred_ptr.
	//
	inline
	void deferred_heap::deregister(const deferred_ptr_void& p) {
		//	no need to actually deregister if we're tearing down this deferred_heap
		if (is_destroying) 
			return;

		//	find its entry, starting from the back because it's more 
		//	likely to be there (newer objects tend to have shorter
		//	lifetimes... all local deferred_ptrs fall into this category,
		//	and especially temporary deferred_ptrs)
		//
		auto erased_count = roots.erase(&p);
		assert(erased_count < 2 && "duplicate registration");
		if (erased_count > 0)
			return;

		for (auto& pg : pages) {
			auto j = find_if(pg.deferred_ptrs.rbegin(), pg.deferred_ptrs.rend(),
				[&p](auto x) { return x.p == &p; });
			if (j != pg.deferred_ptrs.rend()) {
				*j = pg.deferred_ptrs.back();
				pg.deferred_ptrs.pop_back();
				return;
			}
		}

		assert(!"attempt to deregister an unregistered deferred_ptr");
	}

	//  Return the dhpage on which this object exists.
	//	If the object is not in our storage, returns null.
	//
	template<class T>
	deferred_heap::dhpage* deferred_heap::find_dhpage_of(T* p) noexcept {
		for (auto& pg : pages) {
			if (pg.page.contains(p))
				return &pg;
		}
		return nullptr;
	}

	template<class T>
	deferred_heap::find_dhpage_info_ret deferred_heap::find_dhpage_info(T* p)  noexcept {
		find_dhpage_info_ret ret;
		for (auto& pg : pages) {
			auto info = pg.page.contains_info(p);
			if (info.found != gpage::not_in_range) {
				ret.page = &pg;
				ret.info = info;
			}
		}
		return ret;
	}

	template<class T>
	T* deferred_heap::allocate_from_existing_pages(std::size_t n) {
		T* p = nullptr;
		for (auto& pg : pages) {
			p = pg.page.allocate<T>(n);
			if (p != nullptr)
				break;
		}
		return p;
	}

	template<class T>
	deferred_ptr<T> deferred_heap::allocate(std::size_t n) 
	{
		//	get raw memory from the backing storage...
		T* p = allocate_from_existing_pages<T>(n);

		//	... performing a collection if necessary ...
		if (p == nullptr && collect_before_expand) {
			collect();
			p = allocate_from_existing_pages<T>(n);
		}

		//	... allocating another page if necessary
		if (p == nullptr) {
			//	pass along the type hint for size/alignment
			pages.emplace_back((T*)nullptr, n);
			p = pages.back().page.allocate<T>(n);
		}

		assert(p != nullptr && "failed to allocate but didn't throw an exception");
		return p;
	}

	template<class T, class ...Args>
	void deferred_heap::construct(T* p, Args&& ...args) 
	{
		assert(p != nullptr && "construction at null location");

		//	if there are objects with deferred destructors in this
		//	region, run those first and remove them
		destroy_objects((byte*)p, (byte*)(p + 1));

		//	construct the object...

		//	=====================================================================
		//  === BEGIN REENTRANCY-SAFE: ensure no in-progress use of private state
		::new (p) T{ std::forward<Args>(args)... };
		//  === END REENTRANCY-SAFE: reload any stored copies of private state
		//	=====================================================================

		//	... and store the destructor
		dtors.store(p);
	}

	template<class T>
	void deferred_heap::construct_array(T* p, std::size_t n) 
	{
		assert(p != nullptr && "construction at null location");

		//	if there are objects with deferred destructors in this
		//	region, run those first and remove them
		destroy_objects((byte*)p, (byte*)(p + n));

		//	construct all the objects...

		for (std::size_t i = 0; i < n; ++i) {
			//	=====================================================================
			//  === BEGIN REENTRANCY-SAFE: ensure no in-progress use of private state
			::new (p) T{};
			//  === END REENTRANCY-SAFE: reload any stored copies of private state
			//	=====================================================================
		}

		//	... and store the destructor
		dtors.store(p, n);
	}

	template<class T>
	void deferred_heap::destroy(T* p) noexcept 
	{
		assert((p == nullptr || dtors.is_stored(p))
			&& "attempt to destroy an object whose destructor is not registered");
	}

	inline
	bool deferred_heap::destroy_objects(byte* start, byte* end) {
		return dtors.run(start, end);
	}

	//------------------------------------------------------------------------
	//
	//	collect, et al.: Sweep the deferred heap
	//
	inline
	void deferred_heap::mark(const void* p, std::size_t level) noexcept
	{
		// if it isn't null ...
		if (p == nullptr)
			return;

// TODO -- better replacement for rest of this function
		////	... find which page it points into ...
		//auto where = find_dhpage_info(&p);
		//assert(where.page != nullptr
		//	&& "must not mark a location that's not in our heap");

		//// ... mark the chunk as live ...
		//where.page.live_starts.set(where.start_location, true);

		////	... and mark any deferred_ptrs in the allocation as reachable
		//for (auto& dp : deferred_ptrs) {
		//	// TODO this is inefficient, clean up
		//	auto dp_where = where.page.page.contains_info((byte*)dp.p);
		//	if (dp_where.found != gpage::not_in_range
		//		&& dp_where.start_location == where.info.start_location
		//		&& dp.level == 0) {
		//		dp.level = level;	// 'level' steps from a root
		//	}
		//}


		// ... find which page it points into ...
		for (auto& pg : pages) {
			auto where = pg.page.contains_info((byte*)p);
			assert(where.found != gpage::in_range_unallocated
				&& "must not point to unallocated memory");
			if (where.found != gpage::not_in_range) {
				// ... and mark the chunk as live ...
				pg.live_starts.set(where.start_location, true);

				// ... and mark any deferred_ptrs in the allocation as reachable
				for (auto& dp : pg.deferred_ptrs) {
					auto dp_where = pg.page.contains_info((byte*)dp.p);
					assert((dp_where.found == gpage::in_range_allocated_middle
						|| dp_where.found == gpage::in_range_allocated_start)
						&& "points to unallocated memory");
					if (dp_where.start_location == where.start_location
						&& dp.level == 0) {
						dp.level = level;	// 'level' steps from a root
					}
				}
				break;
			}
		}
	}

	inline
	void deferred_heap::collect()
	{
		//	1. reset all the mark bits and in-arena deferred_ptr levels
		//
		for (auto& pg : pages) {
			pg.live_starts.set_all(false);
			for (auto& dp : pg.deferred_ptrs) {
				dp.level = 0;
			}
		}

		//	2. mark all roots + the in-arena deferred_ptrs reachable from them
		//
		std::size_t level = 1;
		for (auto& p : roots) {
			mark(p->get(), level);	// mark this deferred_ptr root
		}

		bool done = false;
		while (!done) {
			done = true;	// we're done unless we find another to mark
			++level;
			for (auto& pg : pages) {
				for (auto& dp : pg.deferred_ptrs) {
					if (dp.level == level - 1) {
						done = false;
						mark(dp.p->get(), level);	// mark this reachable in-arena deferred_ptr
					}
				}
			}
		}

#ifndef NDEBUG
		//std::cout << "=== COLLECT live_starts locations:\n     ";
		//for (auto& pg : pages) {
		//	for (std::size_t i = 0; i < pg.page.locations(); ++i) {
		//		std::cout << (pg.live_starts.get(i) ? 'A' : '.');
		//		if (i % 8 == 7) { std::cout << ' '; }
		//		if (i % 64 == 63) { std::cout << "\n     "; }
		//	}
		//	std::cout << "\n     ";
		//}
		//std::cout << "\n";
#endif

		//	We have now marked every allocation to save, so now
		//	go through and clean up all the unreachable objects

		//	3. reset all unreached deferred_ptrs to null
		//	
		//	Note: 'const deferred_ptr' is supported and behaves as const w.r.t. the
		//	the program code; however, a deferred_ptr data member can become
		//	spontaneously null *during object destruction* even if declared
		//	const to the rest of the program. So the collector is an exception
		//	to constness, and the const_cast below is because any deferred_ptr must
		//	be able to be set to null during collection, as part of safely
		//	breaking cycles. (We could declare the data member mutable, but
		//	then we might accidentally modify it in another const function.
		//	Since a const deferred_ptr should only be reset in this one case, it's
		//	more appropriate to avoid mutable and put the const_cast here.)
		//
		//	This is the same "don't touch other objects during finalization
		//	because they may already have been finalized" rule as has evolved
		//	in all cycle-breaking approaches. But, unlike the managed languages, here
		//	the rule is actually directly supported and enforced (one object
		//	being destroyed cannot touch another deferred-cleanup object by
		//	accident because the deferred_ptr to that other object is null), it
		//	removes the need for separate "finalizer" functions (we always run
		//	real destructors, and only have to teach that deferred_ptrs might be null
		//	in a destructor), and it eliminates the possibility of resurrection
		//	(it is not possible for a destructor to make a collectable object
		//	reachable again because we eliminate all pointers to it before any
		//	user-defined destructor gets a chance to run). This is fully
		//	compatible with learnings from existing approaches, but strictly
		//	better in all these respects by directly enforcing those learnings
		//	in the design, thus eliminating large classes of errors while also
		//	minimizing complexity by inventing no new concepts other than
		//	the rule "deferred_ptrs can be null in dtors."
		//
		for (auto& pg : pages) {
			for (auto& dp : pg.deferred_ptrs) {
				if (dp.level == 0) {
					const_cast<deferred_ptr_void*>(dp.p)->reset();
				}
			}
		}

		//	4. deallocate all unreachable allocations, running
		//	destructors if registered
		//
		for (auto& pg : pages) {
			for (std::size_t i = 0; i < pg.page.locations(); ++i) {
				auto start = pg.page.location_info(i);
				if (start.is_start && !pg.live_starts.get(i)) {
					//	this is an allocation to destroy and deallocate

					//	find the end of the allocation
					auto end_i = i + 1;
					auto end = pg.page.location_info(pg.page.locations()).pointer;
					for (; end_i < pg.page.locations(); ++end_i) {
						auto info = pg.page.location_info(end_i);
						if (info.is_start) {
							end = info.pointer;
							break;
						}
					}

					// call the destructors for objects in this range
					destroy_objects(start.pointer, end);

					// and then deallocate the raw storage
					pg.page.deallocate(start.pointer);
				}
			}
		}

	}

	inline
	void destructors::debug_print() const {
		std::cout << "\n  destructors size() is " << dtors.size() << "\n";
		for (auto& d : dtors) {
			std::cout << "    " << (void*)(d.p) << ", "
				<< d.n << ", " << (void*)(d.destroy) << "\n";
		}
		std::cout << "\n";
	}

	inline
	void deferred_heap::debug_print() const 
	{
		for (auto& pg : pages) {
			pg.page.debug_print();
			std::cout << "  this page's deferred_ptrs.size() is " << pg.deferred_ptrs.size() << "\n";
			for (auto& dp : pg.deferred_ptrs) {
				std::cout << "    " << (void*)dp.p << " -> " << dp.p->get()
					<< ", level " << dp.level << "\n";
			}
			std::cout << "\n";
		}
		std::cout << "  roots.size() is " << roots.size() 
				  << ", load_factor is " << roots.load_factor() << "\n";
		for (auto& p : roots) {
			std::cout << "    " << (void*)p << " -> " << p->get() << "\n";
		}
		dtors.debug_print();
	}

}

#endif

