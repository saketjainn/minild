/* Cross-file call and cross-file global: exit code is add(counter, 2) = 7. */
extern int counter;
int add(int a, int b);

int main(void) {
  return add(counter, 2);
}
