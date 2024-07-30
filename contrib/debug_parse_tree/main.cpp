#include <iostream>
#include <sstream>
#include "parser.h"

void printUsage()
{
  std::ostringstream oss;
  oss << "Usage (recommended): \n\n $ debug_parse_tree < a_random_sql_file.sql \n\n"
      << "Or, use it directly with stdin. For example,\n\n"
      << "$ debug_parse_tree \n"
      << "> select * from code; \n\n-- press enter twice after to finish.\n";
  std::cout << oss.str();
}

struct TreeWalkContext
{
  int depth;
  std::ostringstream &output;

  TreeWalkContext (std::ostringstream &out) : depth (-1), output (out) {}
};

auto prefunc (PARSER_CONTEXT *p, PT_NODE *tree, void *arg, int *continue_walk) -> PT_NODE *
{
  auto *ctx = static_cast<TreeWalkContext *> (arg);
  ctx->depth++;
  std::string indent (ctx->depth * 4, ' ');
  ctx->output << indent << pt_show_node_type (tree);
  ctx->output << " : " << parser_print_tree (p, tree) << '\n';

  return tree;
}

auto postfunc (PARSER_CONTEXT *p, PT_NODE *tree, void *arg, int *continue_walk) -> PT_NODE *
{
  auto *ctx = static_cast<TreeWalkContext *> (arg);
  ctx->depth--;
  return tree;
}

std::string debug_parse_tree (PARSER_CONTEXT *parser, PT_NODE *node)
{
  std::ostringstream output;
  TreeWalkContext ctx (output);

  output << "Parser Tree Visualization:\n";
  output << "##########################\n\n";
  parser_walk_tree (parser, node, prefunc, &ctx, postfunc, &ctx);
  return output.str();
}

int main (int argc, const char *argv[])
{
  std::ostringstream output;

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
  output << "User Input:\n";
  output << "###########\n\n";
  output << statement << '\n';

  auto node = parser_parse_string (parser, statement.c_str());

  output << debug_parse_tree (parser, *node) << "\n";

  parser_free_parser (parser);

  std::cout << output.str();

  return 0;
}

