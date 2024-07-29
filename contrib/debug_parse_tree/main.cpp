#include <iostream>
#include "parser.h"

void printUsage()
{
  std::cout << "Usage (recommended): \n\n $ debug_parse_tree < a_random_sql_file.sql \n\n"
	    << "Or, use it directly with stdin. For example,\n\n"
	    << "$ debug_parse_tree \n"
	    << "> select * from code; \n\n-- press enter twice after to finish.\n";
}

auto depth = 0;

auto prefunc (PARSER_CONTEXT *p, PT_NODE *tree, void *arg, int *continue_walk) -> PT_NODE *
{
  depth++;
  std::string indent (depth * 4, ' ');
  std::cout << indent << pt_show_node_type (tree);

  std::cout << " : " << parser_print_tree (p, tree) << '\n';

  return tree;
}

auto postfunc (PARSER_CONTEXT *p, PT_NODE *tree, void *arg, int *continue_walk) -> PT_NODE *
{
  depth--;
  return tree;
}

int main (int argc, const char *argv[])
{
  for (int i = 1; i < argc; ++i)
    {
      std::string arg = argv[i];
      if (arg == "-h" || arg == "--help")
	{
	  printUsage();
	  return 0;
	}
    }

  auto parser = parser_create_parser();
  lang_init();
  lang_set_charset (INTL_CODESET_UTF8);
  lang_set_charset_lang ("en_US");

  std::stringstream buffer;
  std::string line;

  while (std::getline (std::cin, line) && !line.empty())
    {
      buffer << line << "\n";
    }

  std::string statement = buffer.str();

  auto node = parser_parse_string (parser, statement.c_str());

  parser_walk_tree (parser, *node, prefunc, nullptr, postfunc, nullptr);

  parser_free_parser (parser);

  return 0;
}
