#if !defined(INSTANCE)
#error INSTANCE not defined
#endif

#if !defined(LIBTYPE)
#error LIBTYPE not defined
#endif

#define CAT2(X, T, I) T ## _ ## X ## _ ## I
#define CAT(X, T, I) CAT2(X, T, I)
#define NS(X) CAT(X, LIBTYPE, INSTANCE)

__thread int NS(tlsvar);
int NS(set)(int val)
{
   NS(tlsvar) = val;
}

int NS(get)()
{
   return NS(tlsvar);
}
