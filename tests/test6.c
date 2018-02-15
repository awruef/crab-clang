
int foo(int a, int b) {
  if (b > 0) {
    int c = 0;

    while (c < b) {
      c += a;
    }

    return c;
  } else {
    return 0;
  }
}
