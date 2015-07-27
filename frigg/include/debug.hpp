
namespace frigg {
namespace debug {

#define ASSERT(c) do { if(!(c)) ::frigg::debug::assertionFail(#c); } while(0)

class LogSink {
public:
	virtual void print(char c) = 0;
	virtual void print(const char *str) = 0;
};

// --------------------------------------------------------
// Logging
// --------------------------------------------------------

template<typename P, typename T>
void printUInt(P &printer, T number, int radix) {
	if(number == 0) {
		printer.print('0');
		return;
	}
	const char *digits = "0123456789abcdef";
	int logarithm = 0;
	T rem = number;
	while(1) {
		rem /= radix;
		if(rem == 0)
			break;
		logarithm++;
	}
	T p = 1;
	for(int i = 0; i < logarithm; i++)
		p *= radix;
	while(p > 0) {
		int d = number / p;
		printer.print(digits[d]);
		number %= p;
		p /= radix;
	}
}


class Finish { };

template<typename P, typename T, typename E = void>
struct Print;

template<typename P>
struct Print<P, Finish> {
	static void print(P &printer, Finish token) {
		printer.finish();
	}
};

template<typename P>
struct Print<P, const char *> {
	static void print(P &printer, const char *string) {
		printer.print(string);
	}
};

template<typename P, typename IntType>
struct Print<P, IntType, typename util::EnableIf<util::IsIntegral<IntType>::value
			&& util::IsSigned<IntType>::value>::type> {
	static void print(P &printer, IntType number) {
		if(number < 0) {
			printer.print('-');
			printUInt(printer, -number, 10);
		}else{
			printUInt(printer, number, 10);
		}
	}
};

template<typename P, typename UIntType>
struct Print<P, UIntType, typename util::EnableIf<util::IsIntegral<UIntType>::value
				&& util::IsUnsigned<UIntType>::value>::type> {
	static void print(P &printer, UIntType number) {
		printUInt(printer, number, 10);
	}
};

template<typename P, typename T>
struct Print<P, T *> {
	static void print(P &printer, T *pointer) {
		printer.print("0x");
		printUInt(printer, (uintptr_t)pointer, 16);
	}
};

template<typename P, typename T,
		typename E = typename P::IsPrinter>
P operator<< (P &&printer, T object) {
	Print<P, T>::print(printer, object);
	return printer;
}

template<typename P, typename T,
		typename E = typename P::IsPrinter>
P &operator<< (P &printer, T object) {
	Print<P, T>::print(printer, object);
	return printer;
}

class DefaultLogger {
public:
	class Printer {
	public:
		struct IsPrinter { };

		Printer(LogSink *sink);

		void print(char c);
		void print(const char *str);

		void finish();

	private:
		LogSink *p_sink;
	};

	DefaultLogger(LogSink *sink);

	Printer log();

private:
	LogSink *p_sink;
};

class PanicLogger {
public:
	class Printer {
	public:
		struct IsPrinter { };

		Printer(LogSink *sink);

		void print(char c);
		void print(const char *str);

		void finish();

	private:
		LogSink *p_sink;
	};

	PanicLogger(LogSink *sink);

	Printer log();

private:
	LogSink *p_sink;
};

extern util::LazyInitializer<PanicLogger> panicLogger;

// --------------------------------------------------------
// Namespace scope functions
// --------------------------------------------------------

void assertionFail(const char *message);

} } // namespace frigg::debug
